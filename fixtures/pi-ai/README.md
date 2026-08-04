# pi-ai compatibility fixtures

The committed evidence bundle for the pi-ai completion gate ([#347]). Every fixture below is
compared by tests in this repository against the C++ surface, so the gate's evidence is one
checklist away. No fixture value is a live credential or derived from one; all credential-like
strings are distinguishable `dummy-*` tokens (see [Sanitization rules](#sanitization-rules)).

## Pinned baseline and shard artifact

- **Frozen pi commit:** `83114817c68f5413e4d7ba6d7003ddc511cd31d2` (the parity map [#2] baseline).
  The local pi checkout is `../pi`; `pi:` references resolve from that root.
- **Published artifact:** `@earendil-works/pi-ai@0.83.0`, published at pi tag `v0.83.0`
  (commit `845d6ff1f`, released 2026-07-30). The generated provider shards ship as
  `dist/providers/data/<provider>.json` inside the package, alongside `.manifest.json`
  (`structureHash 5d82f5b1946bdf6d01733aa2a4e4410849c6d44a2ad3038171078c17aed367ce`).
  A local install (via `pi-coding-agent@0.83.0`) resolves at
  `<node>/lib/node_modules/@earendil-works/pi-coding-agent/node_modules/@earendil-works/pi-ai`.

### Shard hashes (sha256)

| Shard file | Published `pi-ai@0.83.0` | Baseline-regenerated (`83114817` + `b889a0ce3`) |
| --- | --- | --- |
| `openai-codex.json` | `c3313710bc6910e6bbcb06d5867247e97ec3fa6c2af9bc780f8a3eefb03e32e1` | `4a73818291987693fcb53e9e61b4be3429ffd78b3f64ea877a34c5f9928c89e1` |
| `kimi-coding.json` | `ba42a26a69e5cb2122c0b384ba572efae93f42239ad86f26c2b7320a14aa75a1` | same (no post-release drift) |
| `deepseek.json` | `0dcc807a4e5827b488c6ceac87884ff6e735e01cf4f2ddfec9dd812e6fde041b` | same (no post-release drift) |

**GPT-5.6 pricing divergence (recorded, no code drift):** the published `pi-ai@0.83.0`
`openai-codex.json` predates pi commit `b889a0ce3` *fix(ai): update GPT-5.6 pricing*, which is an
ancestor of the frozen baseline. Only two model cost fields differ between the published artifact
and the baseline source of truth (`packages/ai/scripts/generate-models.ts`,
`OPENAI_GPT_56_STANDARD_COSTS`):

| Model | Published `pi-ai@0.83.0` | Baseline (`83114817`) — what C++ ships |
| --- | --- | --- |
| `gpt-5.6-luna` | `{input: 1, output: 6, cacheRead: 0.1, cacheWrite: 1.25}` | `{input: 0.2, output: 1.2, cacheRead: 0.02, cacheWrite: 0.25}` |
| `gpt-5.6-terra` | `{input: 2.5, output: 15, cacheRead: 0.25, cacheWrite: 3.125}` | `{input: 2, output: 12, cacheRead: 0.2, cacheWrite: 2.5}` |

The C++ catalogs (`src/ai/providers/CodexCatalog.cpp`, `KimiCatalog.cpp`) follow the frozen
baseline's own generated data — the same values the regenerated
`packages/ai/src/providers/data/openai-codex.json` carries at the baseline — so the C++ surface is
baseline-correct and the published artifact's two stale fields are **not** ported. This is an
intentional no-divergence: the baseline commit is the parity authority, and the published package
version is only the commit-correlated artifact reference. Cost values never enter wire bytes, so
this divergence cannot affect request payloads, only cost accounting.

## Sanitization rules

- Every credential-like string in these fixtures is a distinguishable `dummy-*` token
  (`dummy-access-token`, `dummy-refresh-token`, `dummy-deepseek-key`, account id `acc_test`, …).
  No fixture value was copied from a live service, a real `auth.json`, or a real `models.json`.
- Live `~/.pi/agent/auth.json` and `~/.pi/agent/models.json` are compatibility/evidence inputs and
  must never be pasted into fixtures, logs, issues, or commits (parity map [#2]).
- The only non-dummy fixture fields are non-secret structural values (URLs, endpoints, model ids,
  page bodies). The Kimi `auth-storage` fixtures normalize the wall-clock `"expires"` digits to `0`
  before byte comparison because the persisted timestamp is live.
- Secrets (tokens, PKCE verifiers, `accountId`) never enter Model values, session metadata,
  diagnostics, logs, or fixtures — this bundle is a live check on that boundary.

## Fixture inventory

### Models (`models/`)

- `models/default-model.json` mirrors pi Agent's `DEFAULT_MODEL`; omitted optional members mean no
  thinking map, headers, cost tiers, or API compat.
- `models/complete-anthropic-model.json` exercises every supported Model field. Its absent `off`
  thinking key means provider default, while `"low": null` means that level is unsupported.
- `models/models.json` pins the DeepSeek `deepseek-v4-flash` custom-model upsert: a config-only
  `deepseek` provider (`baseUrl`, `api: "openai-responses"`, literal `apiKey`, one custom model with
  `reasoning`/`thinkingLevelMap`/`input`/`cost`) composed by `ModelRuntime.refresh()` purely from
  config plus the privately registered `openai-responses` adapter.

### Conversion, usage, termination (`conversion/`, `usage/`, `termination/`)

These transport-independent goldens land with #339 (the shared conversion/usage/cost layer) and remain
shared by all three scoped adapters:

- `conversion/openai-codex-responses.json`, `conversion/openai-responses-deepseek.json`,
  `conversion/anthropic-messages-kimi.json` pin normalized message histories as the three request
  payload shapes (system-prompt placement, image blocks, encrypted/thinking signatures, Responses
  text-signature v1 replay, normalized tool-call ids, grouped Anthropic tool results with
  `is_error`, adaptive thinking, cache/session fields).
- `usage/normalization-and-cost.json` pins Responses cache subtraction, the DeepSeek
  uncached/cached split, Anthropic partial usage, reasoning details, cost-tier selection, and the
  one-hour cache-write rule.
- `termination/matrix.json` pins the Responses and Anthropic stop-reason matrices, including
  missing-terminal errors.

### Wire goldens (`wire/`)

- `wire/openai-responses-deepseek-ts-request.json` + `.sse` + `-ts-events.json` (#340): the frozen
  DeepSeek `openai-responses` request bytes, raw SSE sequence, and TS assistant event snapshot.
- `wire/anthropic-messages-kimi-ts-request.json` + `.sse` + `-ts-events.json` (#341): the frozen
  Kimi `anthropic-messages` request bytes, raw SSE sequence (including malformed-event and
  incremental-tool JSON repair, partial usage, one-hour cache-write, mandatory `message_stop`), and
  TS event snapshot.
- `wire/openai-codex-responses-ts-request.json` + `.sse` + `-ts-events.json` + `-ws.json` +
  `-ws-ts-events.json` (#342): the frozen Codex `openai-codex-responses` request body (sorted-key
  canonicalized), SSE sequence, TS event snapshots, and the WebSocket frame sequence
  (`response.create` + server events). Documented divergences from the frozen TS bytes: the C++
  adapter omits zstd SSE compression (pi's plain-JSON branch) and sends its own
  `User-Agent: pi (cpp-harness)`.

### Auth goldens (`auth/`, `auth-storage/`)

- `auth/oauth-success-callback.html` and `auth/oauth-error-{route-not-found,state-mismatch,
  missing-code,internal}.html` are the frozen pi `oauthSuccessHtml`/`oauthErrorHtml` page bodies
  captured verbatim from `packages/ai/src/auth/oauth/oauth-page.ts` at the baseline (#343). The
  tests compare the C++ `oauth_success_html`/`oauth_error_html` output byte-for-byte and assert the
  callback server serves the same bodies with pi's exact status codes.
- `auth-storage/round-trip-input.json` + `round-trip-expected.json` pin the shared `auth.json`
  read-modify-write contract: known API-key/OAuth records, Codex `accountId`, unknown provider
  records, and OAuth extra fields survive whole-file persistence under locking (#338).
- `auth-storage/kimi-oauth-after-login.json` + `kimi-oauth-after-refresh.json` pin the persisted
  `auth.json` shapes the Kimi lifecycle writes: the `oauth` record and the rotated record after a
  request-time refresh under the CredentialStore lock. `kimi-oauth-after-refresh.json` also
  documents the no-divergence outcome of a dead credential: logout is the only removal path, and
  dead credentials stay in `auth.json` for retry (#344).

## Differential evidence

Deterministic goldens captured from the frozen pi tests and pi source, with the TS sides pinned as
snapshots in this repository (a node sidecar is an optional enhancement, not a gate requirement):

- Request payload bytes: the `*-ts-request.json` fixtures are the exact bodies the frozen TS
  `buildParams`/`convertResponsesMessages`/`buildRequestBody` produce at the baseline; the C++
  adapters are compared to those bytes through the `Models` seam.
- Event sequences: the `*-ts-events.json` fixtures are TS assistant event-name snapshots; the raw
  `.sse`/`.ws` fixtures are the frozen input sequences. C++ stream output is compared to both.
- Catalog/compat values: `models/` fixtures and the pinned Kimi compat values in
  `AnthropicMessagesAdapterTest` freeze the typed compat semantics.

## Manual evidence (automation-unreachable surfaces)

The only surfaces that cannot be exercised deterministically through the fake-provider seams are
the frozen page/prompt presentation strings. They are pinned as follows:

- **Frozen HTML pages (byte-verified):** the five `auth/oauth-*.html` files above are the verbatim
  `oauthSuccessHtml`/`oauthErrorHtml` bodies; `OpenAICodexOAuthTest` compares the C++ output
  byte-for-byte. Frozen messages inside them: `"OpenAI authentication completed. You can close
  this window."` (success), `"Callback route not found."`, `"State mismatch."`,
  `"Missing authorization code."`, `"Internal error while processing OAuth callback."` (errors),
  matching pi `packages/ai/src/auth/oauth/openai-codex.ts:340–363`.
- **Frozen login presentation content (the ai content layer owns it; the interactive frontend owns
  presentation, ADR 0032):** the prompt and event strings are frozen verbatim from pi
  `packages/ai/src/auth/oauth/openai-codex.ts` / `device-code.ts` / `kimi-coding.ts`:
  - `"Select OpenAI Codex login method:"` with options `"Browser login (default)"` and
    `"Device code login (headless)"` (pi `openai-codex.ts:516–519`).
  - `"A browser window should open. Complete login to finish."` (pi `openai-codex.ts:455`).
  - `"Complete login in your browser, or paste the authorization code / redirect URL here:"` with
    placeholder `http://localhost:1455/auth/callback` (pi `openai-codex.ts:462`).
  - Device flow: `"Login cancelled"`, `"Device flow timed out"`, and the frozen clock-drift message
    `"Device flow timed out after one or more slow_down responses. This is often caused by clock
    drift in WSL or VM environments. Please sync or restart the VM clock and try again."`
    (pi `device-code.ts:1–4`).
  - Kimi failures: `"Kimi Code device authorization expired. Please restart login."`,
    `"Kimi Code login was denied."`, and the `Kimi Code device token request failed …` status
    messages (pi `kimi-coding.ts`).
- No other surface is automation-unreachable: every adapter, OAuth step, and persistence round-trip
  is exercised through scripted fake HTTP/transport/credential-store seams (fake-provider tests per
  the repo validation policy). Live network/credential validation is out of scope for the gate.

## Capability-to-source checklist

One line per scoped capability, tying it to the frozen pi source/shard, the resolution record,
the C++ surface, and the committed evidence. Resolution records: [#326]
`7d813af3650dfa4fd098e90e321fce24`, [#327] `97d28829babcd9a8fa3258031aa8aa03`, [#328]
`4b757195c5e7e5db2ad6d25ab091fe46`, [#329] `746839885c04cf195984af7112f2ea88`, [#330]
`6d06d3172ff3383ed3188a9bef4be587`.

### Supported Capabilities

| # | Capability | Frozen pi source / shard | C++ surface | Evidence (tests → fixtures) |
| --- | --- | --- | --- | --- |
| 1 | Complete passive `Model` (independent `provider`/`id`/`api`, required name/baseUrl/reasoning/input/cost/contextWindow/maxTokens, static headers) | `packages/ai/src/types.ts` | `include/cch/ai/Model.hpp` | `ModelTest` → `models/complete-anthropic-model.json` |
| 2 | Null-aware `thinkingLevelMap` (missing key = provider default, null = unsupported) | `packages/ai/src/types.ts` | `Model.hpp`, `ModelThinkingLevel.hpp` | `ModelTest`, `SimpleOptionsTest` |
| 3 | Typed `AnthropicMessagesCompat` = exactly `{forceAdaptiveThinking, allowEmptySignature}`; no generic compat bag; `OpenAIResponsesCompat` absent | shard `kimi-coding.json`; `types.ts` | `Model.hpp` (`AnthropicMessagesCompat`) | `AnthropicMessagesAdapterTest` `"Kimi catalog carries the frozen Anthropic Messages compat values"` |
| 4 | Concrete Agent `kDefaultModel` mirroring pi `DEFAULT_MODEL` (`"unknown"` identity, zeroed capabilities) | `packages/agent/src/agent.ts` | `src/agent/AgentDefaults.hpp` | `ModelTest` → `models/default-model.json` |
| 5 | `SimpleStreamOptions` harness-consumer set (`temperature`, `maxTokens`, cancellation, `apiKey`, `headers`, `env`, `transformHeaders`, `reasoning`, `sessionId`, `cacheRetention`, `timeoutMs`, `maxRetries`, `maxRetryDelayMs`) | `simple-options.ts`, `agent-harness.ts` | `include/cch/ai/RequestOptions.hpp` | `SimpleOptionsTest`, `ModelsTest` `"Models prepares the complete streamSimple request before Provider dispatch"` |
| 6 | `Models`/`ModelRuntime` two-layer ownership; `ModelRuntime` the sole public `shared_ptr` seam | `models.ts`, `core/model-runtime.ts` | `Models.hpp`, `ModelRuntime.hpp`, `Sdk.hpp` | `ModelsTest`, `ModelRuntimeTest`, `SdkSessionTest` |
| 7 | Agent Config Directory = pi's own (`~/.pi/agent`, `PI_CODING_AGENT_DIR`, SDK `agentDir`); no `CCH_CODING_AGENT_DIR` | `core/config.ts` | `AgentConfigDir.hpp` | `AgentConfigDirTest`, `SessionPathPolicyTest` |
| 8 | `models.json` composition: built-ins → provider overlay/custom-model upsert (same-ID replace) → model overrides; invalid config → empty user config + diagnostics; no global rollback | `core/model-config.ts` | `ModelConfig.hpp`, `ProviderComposer.hpp`, `ModelRuntime.cpp` | `ModelConfigTest`, `ProviderComposerTest`, `ModelRuntimeTest` |
| 9 | Config-only provider composition (DeepSeek `deepseek-v4-flash` from `models.json` + private `openai-responses` adapter) | `models.json` shard + `api/openai-responses.ts` | `ProviderComposer.cpp` | `ModelRuntimeTest` `"ModelRuntime config-only provider streams the frozen deepseek wire path"` → `wire/openai-responses-deepseek-*`, `models/models.json` |
| 10 | Frozen built-in catalogs: Codex 7 (`gpt-5.3-codex-spark`…`gpt-5.6-terra`), Kimi 4 (`k3`, `k3-256k`, `kimi-for-coding`, `kimi-for-coding-highspeed`) with baseline GPT-5.6 pricing | shards `openai-codex.json` / `kimi-coding.json` (baseline-correct) | `CodexCatalog.hpp/.cpp`, `KimiCatalog.hpp/.cpp` | `ProviderComposerTest` `"builtin_providers ships the Codex 7 and Kimi 4 catalogs"`, `AnthropicMessagesAdapterTest` compat values |
| 11 | `openai-codex-responses` adapter: WebSocket-first with narrow SSE fallback, socket reuse (5m idle / 55m age), `previous_response_id` continuation, `store:false`, one retry each for `previous_response_not_found` / pre-start `websocket_connection_limit_reached`, per-session SSE-only marking, one-shot sockets for `cacheRetention:none`, `session-id`/`x-client-request-id`/`chatgpt-account-id` headers | `api/openai-codex-responses.ts` | `src/ai/api/OpenAICodexResponsesAdapter.*`, `BoostBeastWebSocketTransport` | `OpenAICodexResponsesAdapterTest` (19 cases) → `wire/openai-codex-responses-*` |
| 12 | `openai-responses` adapter (DeepSeek stateless full-context replay, no `previous_response_id`, developer-role prompt, session-affinity headers silently ignored) | `api/openai-responses.ts` + `openai-responses-shared.ts` | `src/ai/api/OpenAIResponsesAdapter.*` | `OpenAIResponsesAdapterTest` (7 cases) + `ModelRuntimeTest` → `wire/openai-responses-deepseek-*` |
| 13 | `anthropic-messages` adapter (Kimi): system extraction, image blocks, empty/redacted thinking signature replay via `allowEmptySignature`, adaptive thinking, malformed-event + incremental-tool JSON repair, one-hour cache-write detail, mandatory `message_stop` | `api/anthropic-messages.ts` | `src/ai/api/AnthropicMessagesAdapter.*` | `AnthropicMessagesAdapterTest` (9 cases) → `wire/anthropic-messages-kimi-*` |
| 14 | Message/history conversion alignment per path (system placement, surrogate sanitization, tool-result grouping with `is_error`, tool-call id normalization `[A-Za-z0-9_-]`/64, image blocks, encrypted/text-signature replay) | `utils/transform-messages.ts`, `api/*-shared.ts` | `src/ai/api/MessageConversion.*` | `MessageConversionTest` (7 cases) → `conversion/*.json` |
| 15 | Usage/cost normalization (Responses cached subtraction, Anthropic partial-field updates, DeepSeek uncached/cached split, shared `calculateCost` with tiers + 1h cache-write 2×) | `utils/calculate-cost.ts` etc. | `src/ai/api/UsageNormalization.*`, `src/ai/Usage.cpp` | `UsageTest` (2 cases) → `usage/normalization-and-cost.json` |
| 16 | Termination matrices (Responses `completed`/`incomplete`/`failed`, no `[DONE]` for DeepSeek; Anthropic `end_turn`…/missing `message_stop` carry) | adapter sources | `src/ai/api/Termination.*` | `ProviderPolicyTest` → `termination/matrix.json`; per-adapter terminal cases |
| 17 | Retries: zero by default; configured retries cover network + transient 429/5xx, never terminal quota/billing; exponential base or Retry-After ≤ 60s | `utils/retry.ts` | `src/ai/providers/RetryPolicy.*` | `ProviderPolicyTest`, per-adapter retry cases |
| 18 | Cancellation → exactly one `aborted` terminal event (`"Request was aborted"`); transport/socket/reader cancellation | `stream-fn` semantics | `Models.cpp`, `StreamEvent.hpp`, transports | `ModelsTest` `"Models cancellation is one aborted terminal value"`, per-adapter cancellation cases |
| 19 | Terminal-error-event stream semantics: every setup failure → exactly one terminal error event + final `AssistantMessage`; `Expected` error reserved for sink/infrastructure | `StreamFn` contract | `Models.cpp`, `StreamEvent.hpp` | `ModelsTest`, `FakeProviderTest` `"scripted fake Models normalizes static request failures into a terminal value"` |
| 20 | Six error categories (`model_source`, `model_validation`, `provider`, `stream`, `auth`, `oauth`) through the single `util::Error`/`util::Expected` channel | `models.ts` errors | `cch/util/Error.hpp` | `FakeProviderTest` `"scripted fake Models preserves all six Models error categories"` |
| 21 | `streamSimple` pipeline: `getSupportedThinkingLevels`/`clampThinkingLevel`, Responses effort mapping, Anthropic adaptive branch (temperature omitted while thinking), `clampMaxTokensToContext` (context − estimate − 4096), `assertRequestAuth` | `simple-options.ts`, adapters | `src/ai/SimpleOptions.cpp`, adapters | `SimpleOptionsTest`, per-adapter thinking cases |
| 22 | Four-level auth precedence: runtime override → stored credential → environment → models.json configured key; no silent fallback on mismatch/refresh failure | `core/model-runtime.ts`, `auth/resolve.ts` | `Models.cpp`, `RuntimeApiKeyOverlay.*` | `ModelsTest` `"Models applies explicit stored and ambient API key precedence"`, `ModelRuntimeTest` `"ModelRuntime resolves the pi 4-level auth precedence chain"` |
| 23 | Request-time `getAuth` live resolution: OAuth refresh under store lock (≤5 min validity, double-check), persist before release, `oauth` category on failure with credential preserved; `checkAuth` side-effect-free | `auth/resolve.ts` | `Models.cpp` | `ModelsTest` `"Models refreshes OAuth under the store mutation and checkAuth never refreshes"`, `KimiOAuthLifecycleTest` |
| 24 | `CredentialStore` interface (`read`, metadata-only `list`, serialized `modify`, `remove`) + file-backed `AuthStorage` with whole-file proper-lockfile-compatible locking, `0o700`/`0o600`, lossless unknown-record preservation, last-valid-read | `core/auth-storage.ts` | `CredentialStore.hpp`, `AuthStorage.hpp` | `AuthStorageTest` (7 cases) → `auth-storage/round-trip-{input,expected}.json` |
| 25 | Explicit-only login via injected cancellable `AuthInteraction` (prompt/notify hooks, per-prompt stop token); persisted via `CredentialStore::modify`; never implicit | `auth/types.ts` | `Auth.hpp`, `Models.cpp` | `ModelsTest` login cases, `KimiOAuthLifecycleTest` |
| 26 | Codex PKCE S256 callback-server login on `127.0.0.1:1455` (`PI_OAUTH_CALLBACK_HOST`), callback-vs-manual-code race, unverified-JWT `accountId`, `originator=pi` | `auth/oauth/openai-codex.ts` | `OpenAICodexOAuth.*`, `OAuthCallbackServer.*`, `Pkce.*` | `OpenAICodexOAuthTest` (21 cases) → `auth/oauth-*.html` |
| 27 | Kimi RFC 8628 device flow: `verification_uri_complete` (http(s)-only), 5s/15min defaults, wait-before-first-poll, `expired_token`/`access_denied`/`slow_down`, 30s per-request timeout, `KIMI_CODE_OAUTH_HOST` | `auth/oauth/kimi-coding.ts`, `device-code.ts` | `KimiCodingOAuth.*`, `DevicePoll.hpp` | `KimiCodingOAuthTest` (16 cases) |
| 28 | Login cancellation → stable `Cancelled` kind + `"Login cancelled"`; Kimi request-path refresh uncancellable (no-divergence) | `auth/oauth/*`, `resolve.ts` | `DevicePoll.hpp`, `Models.cpp` | `OpenAICodexOAuthTest`, `KimiCodingOAuthTest`, `KimiOAuthLifecycleTest` → `auth-storage/kimi-oauth-after-refresh.json` |
| 29 | Logout = local removal only (no revocation); dead/expired credentials stay in `auth.json` and every request fails with re-auth guidance | `models.ts` logout | `Models.cpp`, `ModelRuntime.cpp` | `KimiOAuthLifecycleTest` `"Kimi dead credentials stay in auth.json"`, `AuthStorageTest` |
| 30 | Secret boundary: secrets never enter Model/session/diagnostics/logs/fixtures; `CredentialStore::list` metadata-only | `#327` resolution | `Auth.hpp`, `CredentialStore.hpp` | `AuthStorageTest`, session/redaction tests |
| 31 | `settings.json` two-scope contract (global + trusted project, deep merge project-wins, global-only `defaultProjectTrust`, pi read-time migrations, surgical field-level writes, no schema markers) | `core/settings-manager.ts` | `Settings.hpp`, `SettingsManager.cpp` | `SettingsManagerTest`, `CliSmokeTest` |
| 32 | CLI flag surface `--provider`/`--model`/`--models`/`--api-key`; `--auth`/`--api-key-env`/`--base-url` removed and fail loudly; resume persists only `model_change {provider, modelId}` re-resolved live | `cli/args.ts`, `core/agent-session.ts` | `CliParse.cpp`, `SessionResume.hpp` | `CliParseTest`, `CliSmokeTest`, `AgentSessionSnapshotTest` |

### Deferred Capabilities (absent from the surface — no placeholders, no compatibility shims)

- Every pi adapter other than the three scoped ones (`openai-completions`, `openai-responses`
  (generic), `anthropic-messages` for non-Kimi providers, `mistral-conversations`, `gemini`,
  `claude-code`, `codex-cli`, images, bedrock, etc.); there is no registry placeholder.
- Every provider family other than `openai-codex`, `deepseek`, `kimi-coding`, including their
  factories, auth methods, and catalogs.
- `OpenAIResponsesCompat` (all seven pi fields fixed at frozen defaults/auto-detection; the typed
  struct does not exist), `serviceTier` (usage multiplier unreachable), `reasoningSummary` (fixed
  `"auto"`), `toolChoice` (fixed `"auto"`), `metadata`, `onPayload`/`onResponse`, `thinkingBudgets`,
  `transport`, `websocketConnectTimeoutMs` (internal fixed 15s), raw per-API option structs.
- `models.json` `compat` overrides; grammar emission; tool search/`splitDeferredTools`/
  `addedToolNames` replay; the Claude subscription branch (Kimi OAuth arrives as a caller-owned
  `Authorization` header); budget-based thinking (`thinkingBudgetTokens` — all scoped Kimi models
  are adaptive); strict-mode overrides; session-affinity format overrides; zstd SSE compression
  (pi's plain-JSON branch).
- Codex catalog flags `supportsOpenAIGrammarTools`/`supportsToolSearch` (no scoped producer).
- Interactive login presentation (LoginDialog/OAuthSelector rendering, `/login` `/logout` commands,
  browser opening) — owned by the later pi-coding-agent gate, not by `cch_ai` (ADR 0032).
- End-to-end agent-harness-through-`streamSimple`, TUI, and CLI full-chain acceptance — explicitly
  NOT this gate (belongs to pi-agent-core / pi-tui / pi-coding-agent, in module order).

## Final classification

Every capability scoped by the parity map for the three provider/auth paths
(`openai-codex` → OAuth → `openai-codex-responses`; `deepseek` → api key + `deepseek-v4-flash` →
`openai-responses`; `kimi-coding` → OAuth → `anthropic-messages`) is either a **Supported
Capability** carrying the evidence in the checklist above, or a **Deferred Capability** absent from
the surface. There are no partial placeholders, no compatibility shims, and no fallback reads: the
legacy `ProviderRegistry`/`ProviderFactoryContext`/`AuthLoader`/`ProviderConfigResolution` and the
`--auth`/`--api-key-env`/`--base-url` flags are removed from the surface. Full test suite:
`1440 test(s), 0 failure(s)` at the completion gate.

## Gate report

### Per-capability evidence map

- **Adapters** — request payload bytes, SSE/WS event sequences, TS event snapshots, terminal
  matrix, and usage/cost normalization are all covered by deterministic goldens:
  `OpenAICodexResponsesAdapterTest` (19), `OpenAIResponsesAdapterTest` (7),
  `AnthropicMessagesAdapterTest` (9), `ProviderPolicyTest` (2), `UsageTest` (2),
  `MessageConversionTest` (7), plus the `ModelRuntime` re-drive of the DeepSeek wire path.
- **OAuth lifecycle** — login, refresh, logout, cancellation, and expiry-at-request-time are covered
  per provider: `OpenAICodexOAuthTest` (21), `KimiCodingOAuthTest` (16),
  `KimiOAuthLifecycleTest` (4), and persistence under locking in `AuthStorageTest` (7).
- **Models/stream semantics** — terminal-error-event + final message, one `aborted` terminal, six
  categories, precedence, and side-effect-free `checkAuth`: `ModelsTest` (27), `FakeProviderTest`
  (7), `SimpleOptionsTest` (2).
- The six-category evidence lives in `tests/ai/providers/FakeProviderTest.cpp`, not in
  `ModelsTest`/`SimpleOptionsTest` (which cover terminal/cancellation but not the category loop).

### Residual notes

1. **Built-in catalog values are pinned in source, not by a committed shard golden.** The catalogs
   (`CodexCatalog.cpp`/`KimiCatalog.cpp`) match the frozen baseline `generate-models.ts` source and
   the baseline-regenerated shard; the GPT-5.6 pricing divergence from the published artifact is
   recorded above. A future gate may add a full catalog byte-golden; the current tests assert ids,
   boundaries, and the Kimi compat values.
2. **Kimi OAuth exact defaults (1s/2s/4s refresh backoff base, 30s per-request timeout) are pinned
   in source (`KimiCodingOAuth.hpp`), not wall-clock asserted.** The exponential-doubling, 4-attempt
   ceiling, and timeout-composition mechanisms ARE asserted with injected small values.
3. **Codex `accountId` is asserted via the `chatgpt_account_id` claim and request headers, not as a
   byte-for-byte frozen JWT.** No fixture pins the JWT itself; the claim value is the dummy
   `acc_test`.
4. **POSIX permission assertions are platform-gated** (`AuthStorageTest` `SUCCEED`s on non-Unix);
   the `0o700`/`0o600` surface is unverified on Windows, consistent with the current platform scope.
5. **No live-network or real-credential validation** — all evidence is fake-provider/scripted
   transport per the repo validation policy. Live smoke (`CCH_LIVE_KIMI=1`) remains optional and
   manual.
6. **Documentation consistency completed:** ADR 0033, the ADR 0019/0029 refinements, and the
   CONTEXT.md entries (Adapter, Compat Field, Transport, Session Affinity, Auth Interaction, OAuth
   Login) verified against the landed surface with no drift. Two README drifts found and fixed in
   this gate: the session-dir env var is now pi's `PI_CODING_AGENT_SESSION_DIR`
   (`CCH_CODING_AGENT_SESSION_DIR` removed, matching ADR 0031), and the README no longer lists OAuth
   as deferred nor pins the parity baseline at the pre-advance `864b35c`.

### Handoff surface to pi-agent-core (#330 decision 4, #331 scope unchanged)

The pi-agent-core gate consumes exactly this surface (the #336–#346 contract surface per #330
decision 4; #331 froze the Agent/Agent-Turn scope that builds on it):

- `streamSimple` request options (`temperature`, `maxTokens`, cancellation, `apiKey`, `headers`,
  `env`, `transformHeaders`, `reasoning`, `sessionId`, `cacheRetention`, `timeoutMs`, `maxRetries`,
  `maxRetryDelayMs`) and the terminal-error-event contract with the six-category channel.
- `ModelRuntime` as the sole injectable seam (`std::shared_ptr` in `CreateAgentSessionOptions`),
  with `checkAuth`/`getAuth`/`login`/`logout`/`set_runtime_api_key`/`refresh` live for all holders.
- Request-time auth resolution (no OAuth snapshot at session construction), the frozen secret
  boundary, and the `"Run '/login <provider>' to re-authenticate."` re-auth guidance owned by the
  session layer.
- This gate does NOT attempt E2E: agent-harness-through-`streamSimple`, TUI login presentation, and
  CLI full-chain acceptance belong to the downstream module gates in order.

[#2]: https://github.com/lanshengzhi/cpp-coding-harness/issues/2
[#326]: https://github.com/lanshengzhi/cpp-coding-harness/issues/326
[#327]: https://github.com/lanshengzhi/cpp-coding-harness/issues/327
[#328]: https://github.com/lanshengzhi/cpp-coding-harness/issues/328
[#329]: https://github.com/lanshengzhi/cpp-coding-harness/issues/329
[#330]: https://github.com/lanshengzhi/cpp-coding-harness/issues/330
[#331]: https://github.com/lanshengzhi/cpp-coding-harness/issues/331
[#347]: https://github.com/lanshengzhi/cpp-coding-harness/issues/347
