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

Issue #342 adds the Codex `openai-codex-responses` wire goldens (WebSocket-first):

- `wire/openai-codex-responses-ts-request.json` is the canonical sorted-key byte snapshot of the Codex request body
  produced by frozen TS `streamSimple`/`buildRequestBody`/`convertResponsesMessages` at
  `packages/ai/src/api/openai-codex-responses.ts` for the same inputs the C++ test drives through the `Models`
  seam. It covers the `instructions` system prompt, `include:["reasoning.encrypted_content"]`, `prompt_cache_key`
  from `sessionId`, the fixed compat defaults (`store:false`, `parallel_tool_calls:true`, `text.verbosity`,
  `tool_choice:"auto"`, `strict:null` tools), and the xhigh reasoning effort. The bytes are canonicalized to the
  C++ sorted-key serialization (as the DeepSeek fixture is); field-level semantics were captured by running the
  frozen TS against the same inputs and comparing the canonicalized objects. The WS frame wraps this body with
  `"type":"response.create"` (see `wire/openai-codex-responses-ws.json`).
- `wire/openai-codex-responses.sse` pins the frozen Codex SSE event sequence, including the encrypted reasoning
  item replayed as a signature, the `phase:"final_answer"` text-signature-v1 item, the function-call delta stream,
  cached/cache-write usage, and the trailing `[DONE]` sentinel. `wire/openai-codex-responses-ts-events.json` is
  the corresponding TS event-name snapshot compared through the `Models` seam.
- `wire/openai-codex-responses-ws.json` pins the WebSocket wire sequence: the client `response.create` frame and
  the server event frames. `wire/openai-codex-responses-ws-ts-events.json` is the matching event-name snapshot.
- The `session-id`/`x-client-request-id` headers derived from `sessionId` are asserted in the adapter tests
  (WS and SSE), along with `chatgpt-account-id`, `originator: pi`, and the SSE `OpenAI-Beta:
  responses=experimental` value. The WS handshake deliberately carries no `OpenAI-Beta` (pi's
  `connectWebSocket` deletes it). The C++ adapter omits zstd SSE request compression deliberately
  (pi's plain-JSON branch) and sends its own `User-Agent: pi (cpp-harness)`; both are documented
  divergences from the frozen TS bytes.
- Adapter tests cover the WebSocket-first lifecycle: connect failure/idle-before-start SSE fallback with the
  recorded transport diagnostic, post-start failure surfacing without fallback, one retry each for
  `previous_response_not_found` and pre-start `websocket_connection_limit_reached`, per-session SSE-only
  marking, socket reuse with `previous_response_id` input deltas, idle-close and hard-age expiry, one-shot
  sockets for `cacheRetention: "none"`, the Codex terminal matrix, cancellation closing the socket with one
  `aborted` terminal, and the JWT `chatgpt_account_id` extraction. All credential-like values are
  distinguishable `dummy-*` values; the JWT carries the fixture account id `acc_test` and no live secret.

Issue #343 adds the OAuth login content layer (AuthInteraction contract + Codex PKCE callback-server login):

- `auth/oauth-success-callback.html`, `auth/oauth-error-route-not-found.html`, `auth/oauth-error-state-mismatch.html`,
  `auth/oauth-error-missing-code.html`, and `auth/oauth-error-internal.html` are the frozen pi
  `oauthSuccessHtml`/`oauthErrorHtml` page bodies captured verbatim from `packages/ai/src/auth/oauth/oauth-page.ts`
  at baseline `83114817`. The tests compare the C++ `oauth_success_html`/`oauth_error_html` output byte-for-byte
  against these files, and assert the callback server serves the same bodies with pi's exact status codes.
- The Codex browser-login flow is exercised with a scripted `AuthInteraction` against a scripted fake HTTP client
  and a real loopback callback server on an ephemeral port: the callback-vs-manual-code race in both directions,
  the frozen authorize-URL bytes (`originator=pi`, PKCE S256 challenge, 16-byte hex state), the token-exchange
  form body with the frozen `redirect_uri=http://localhost:1455/auth/callback`, unverified-JWT `accountId`
  extraction, cancellation normalizing to `util::ErrorCode::Cancelled` + "Login cancelled", listen-failure
  degrade to manual entry, the `PI_OAUTH_CALLBACK_HOST` bind override, and refresh errors carrying status + body
  with no stderr output. All credential-like values are distinguishable `dummy-*` values; no fixture value came
  from a live service.

Issue #344 adds the Kimi Code RFC 8628 device-flow OAuth content layer (`auth/oauth/kimi-coding.ts` at the
frozen baseline, with the shared device-poll helper from `auth/oauth/device-code.ts`):

- `auth-storage/kimi-oauth-after-login.json` and `auth-storage/kimi-oauth-after-refresh.json` pin the persisted
  `auth.json` shapes the Kimi lifecycle writes: the `oauth` record with `refresh`/`access`/`expires` and the
  rotated record after a request-time refresh under the CredentialStore lock. Because `expires` is a live wall-
  clock timestamp, the fixtures use a `0` sentinel and the tests normalize the digits after `"expires":` to `0`
  before comparing bytes. `kimi-oauth-after-refresh.json` also documents the no-divergence outcome of a dead
  credential: logout is the only removal path, and dead credentials stay in `auth.json` for retry.
- The Kimi OAuth tests drive the full lifecycle through `Models` + the real file-backed `AuthStorage` against a
  scripted fake HTTP client: login persists through `CredentialStore::modify`, request-time refresh rotates the
  credential under the store lock and persists before release, `Models::logout` removes the record locally, and
  cancellation persists nothing and normalizes to "Login cancelled". The content-layer tests pin the frozen
  device-flow messages (`verification_uri_complete` http(s)-only validation, 5s/15min interval/expires defaults,
  wait-before-first-poll, `expired_token`/`access_denied`/`slow_down` outcomes, the composed 30s per-request
  timeout, `KIMI_CODE_OAUTH_HOST` override) and the refresh contract (1/2/4s exponential backoff across at most
  four attempts, immediate failure on 401/403/`invalid_grant`, 429/5xx and transport retries, and `toAuth`
  deriving `Authorization: Bearer <access>`). All credential-like values are distinguishable `dummy-*` values;
  no fixture value came from a live service.

Issue #345 adds the `ModelRuntime` composition seam and the first full vertical path (models.json → config-only
provider → stream):

- `models/models.json` pins the DeepSeek `deepseek-v4-flash` custom-model upsert: a config-only `deepseek`
  provider (`baseUrl`, `api`, literal `apiKey`, one custom model with `reasoning`/`thinkingLevelMap`/`input`/
  `cost`) composed by `ModelRuntime.refresh()` purely from config plus the privately registered
  `openai-responses` adapter. The fixture drives the vertical-path test in `ModelRuntimeTest`, which streams
  through the `ModelRuntime` seam with an injected scripted transport and matches the #340
  `wire/openai-responses-deepseek-*` goldens byte-for-byte (request body, event sequence, usage). All
  credential-like values are distinguishable `dummy-*` values.
- The `ModelRuntime` tests cover built-in/config composition (Codex 7 + Kimi 4 catalogs, same-ID custom-model
  upsert, model overrides, built-in fallback on per-provider composition failure), `refresh()` failure
  semantics (invalid config → empty user config + diagnostics; no global rollback), config-value resolution
  (literal, `$VAR`/`${VAR}` env templates, `!command`), login/logout delegation with composition-errors-map
  refresh failures that never fail the call, the availability snapshot, and the frozen default-model table
  (`openai-codex → gpt-5.5`, `kimi-coding → kimi-for-coding`; config-only deepseek takes the "no default
  model" branch). The fixture-agnostic ModelConfig/ProviderComposer tests pin the parsing/validation and
  composition contracts against the same frozen baseline.
