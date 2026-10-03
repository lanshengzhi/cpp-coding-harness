# cch_ai ↔ pi-ai v1.0.0 子集差异审计报告

**Date:** 2026-10-03
**Baseline (audit target):** pi `packages/ai/src` @ `a13d35a74` (v1.0.0). The audit's detailed source reading was performed at `005af57d88ee23b33778f343a9595b32e67ff788` (v0.99.2); v1.0.0 adds exactly three pi-ai commits on top (verified by `git log 005af57d8..a13d35a74 -- packages/ai/src`), all triaged below, and re-verification confirmed the ChatGPT sign-in surface is byte-identical at v1.0.0.
**Prior frozen baseline:** `f07218c4` (v0.87.1), the repo's T1 fixture provenance pin. Verified ancestor of `005af57d8`.
**Subject:** this repository's `cch_ai` Owner (`include/cch/ai/`, `src/ai/`) and its evidence bundle `fixtures/pi-ai/README.md`.
**Method and partition design:** `docs/research/pi-ai-v0.99.2-subset-audit-design.md`. Decision trail: `docs/research/pi-ai-v0.99.2-subset-audit-decisions.tsv`.

## Verdict summary

- **GAP (red):** 2 confirmed, both wire/transport-visible.
- **UPSTREAM-DRIFT scope calls (blue):** 6 clusters, each needing a port-or-defer decision.
- **UPSTREAM-DRIFT display-only:** 1 (provider display name).
- **Pre-existing divergence confirmed accurate:** all previously recorded yellow rows survived re-audit unchanged.
- **Coverage:** all 193 files under `packages/ai/src` @ v0.99.2 assigned to a partition; all 21 drift-window commits triaged. No unclassified file, no untriaged commit.

The two red GAPs and the blue clusters below are the entire actionable drift output. Everything else in the four scoped adapters and the cross-cutting layer is ALIGNED at the frozen-baseline semantics, except one self-contained drift row (B-4) whose C++ side matches the old frozen matrix while the upstream side has since refined the behavior; B-4 is counted in the blue rows above, not double-counted here.

---

## GAP (red) — confirmed by judge spot-check

### GAP-1: Responses adapter lacks the unfinished-tool-call terminal guard

- **pi:** `packages/ai/src/api/openai-responses-shared.ts:767-776` @005af57d8 (guard body; the explanatory comment opens at :764). After the stream loop, when the terminal stop reason is `toolUse`, pi walks the final message's toolCall blocks and throws if any still carries `partialJson`/`customInput` scratch, refusing to hand cut-off or mixed-up arguments to the agent. Introduced by upstream commit `1b2aa0ca0` ("reject unfinished Responses tool calls instead of running them").
- **C++:** `src/ai/api/ResponsesEventProcessor.cpp:284-290`. `finish()` checks only `saw_terminal`; nothing inspects tool-call slot scratch at end of stream. A truncated or non-compliant server response would hand unfinished calls to the agent, exactly what the upstream fix prevents.
- **Class:** UPSTREAM-DRIFT-induced GAP on an in-scope surface (ADR 0059 lists `openai-responses-shared.ts` as a scoped reference).
- **Disposition:** port the guard into `ResponsesEventProcessor::finish()` and pin it with a frozen golden (upstream test `openai-responses-terminal-event.test.ts` has a `createUnfinishedToolCallEvents` scenario to capture against).
- **Status (2026-10-03): PORTED.** Guard landed in `ResponsesEventProcessor::finish()` (commit `105eae3ec`, no tracking issue); repro test plus frozen wire fixture `fixtures/pi-ai/wire/openai-responses-deepseek-unfinished-tool-call.sse` pin it. The C++ pre-port `finish()` at `ResponsesEventProcessor.cpp:284-290` checked only `saw_terminal`, as cited above.

### GAP-2: Codex WS omits `session-id` / `x-client-request-id` when no session is set

- **pi:** `packages/ai/src/api/openai-codex-responses.ts:281-283` @005af57d8. `websocketRequestId = codexSessionId || uuidv7()` — the headers are always sent on the WS handshake; with no session the id is a fresh uuidv7. On the SSE path, `buildSSEHeaders` receives the (possibly empty-string) `codexSessionId` and always sets both headers.
- **C++:** `src/ai/api/OpenAICodexResponsesAdapter.cpp:278-285`. Both headers are added only `if (options.session_id)`; otherwise omitted entirely.
- **Class:** pre-existing GAP, present identically at `f07218c4` (not v0.99.2 drift). Unrecorded in `fixtures/pi-ai/README.md` residual notes.
- **Disposition:** decide whether to mirror pi (always send, uuidv7 fallback on WS; empty-string on SSE) or record a divergence. Wire-byte visible; either way needs a golden update. Note the SSE nuance: pi sends the headers with an *empty-string* value when no session; C++ omits the header entirely — these are different bytes and the choice must be deliberate.
- **Status (2026-10-03): PORTED (issue #863).** WebSocket now always sends `session-id`/`x-client-request-id`, falling back to a fresh RFC 4122 v4 UUID when no session is set (uuidv4 chosen over uuidv7 because the value is opaque to the backend and the repo has no v7 generator). The SSE path keeps the conditional shape: `clampOpenAIPromptCacheKey(undefined)` returns `undefined`, so `buildSSEHeaders`' `if (sessionId)` skips both headers at the empty set, and the C++ mirrors that. Golden tests `Codex always sends session-id and x-client-request-id on the WebSocket handshake` and `Codex omits session-id and x-client-request-id on SSE when no session is set` pin both shapes.

---

## UPSTREAM-DRIFT scope calls (blue) — port or defer, cannot stay silent

The frozen baseline is `f07218c4`. v0.99.2 introduces the following on or adjacent to scoped surfaces. None is recorded in the fixture README Supported rows or Deferred list; each needs an explicit decision (port + golden, or Deferred-list entry) before the baseline can advance.

| # | Cluster | Introduced by | Scoped surface? | Decision needed |
| --- | --- | --- | --- | --- |
| B-1 | **Sign in with ChatGPT** for the `openai` provider: new `auth/oauth/openai-chatgpt.ts` (+310 lines: dynamic client registration, `resource.invoke` scope, 3-min expiry margin, device UUID), `LoginOptions`/`getDeviceId` on `Models.login`, ChatGPT-token request shaping in `api/openai-responses.ts` (omit cache fields / `max_output_tokens` / `temperature` for non-`sk-` credentials), `subscription_sharing_usage_limit_exceeded` usage-hint error string | `02eed88fd` | Adjacent — touches in-scope `openai` provider family and the in-scope Responses adapter's request shaping | Biggest call in this audit. Port requires amending ADR 0032's frozen `Models::login` signature plus a new OAuth flow and frozen fixtures. Defer requires a Deferred-list entry. |
| B-2 | **Anthropic workload identity federation**: `ANTHROPIC_FEDERATION_RULE_ID` / `ORGANIZATION_ID` / `IDENTITY_TOKEN_FILE` env vars, `PiAnthropic` client bypassing the SDK credential chain, `federationClient` reuse; bypasses the "No API key" throw for provider `anthropic` | `a9424cd43` | Adjacent — Anthropic adapter is scoped, but federation scopes only the `anthropic` provider with an SDK token exchange C++ has no equivalent of | Port = new env-var auth surface + token exchange. Defer = Deferred-list entry. Low urgency: no scoped bundled provider is `anthropic`. |
| B-3 | **Retry classification additions**: `subscription_sharing_usage_limit_exceeded` (non-retryable), `subscription_sharing_usage_unavailable` / `subscription_sharing_user_unavailable` (retryable) | `02eed88fd` (in-range hunk on `utils/retry.ts`) | In-scope file (`RetryPolicy.*`) | Minor. C++ `RetryPolicy.cpp` lacks these strings; today they classify as default non-retryable. Reachable only via ChatGPT-subscription tokens, which no bundled provider can produce today (the bundled set is eight providers: DeepSeek, OpenRouter, OpenCode Go, OpenAI, OpenAI Codex, Kimi, plus the Anthropic-messages and generic Responses surfaces; none route ChatGPT-subscription auth). Severity is low. Port with B-1 or defer together. |
| B-4 | **`incomplete` stop-reason refinement**: `mapStopReason` now splits `incomplete.max_output_tokens` → `length` from other `incomplete` → `error` with the specific provider reason (e.g. content-filter), and `rawStopReason` is now `status.reason` composite rather than bare status | post-`f07218c4` refinement of `openai-responses-shared.ts` | In-scope (Responses/Anthropic termination) | C++ `Termination.cpp:26` maps all `incomplete` → `Length` and `ResponsesEventProcessor.cpp:112-115` records bare `status` as `raw_stop_reason`. Frozen matrix fixture (`termination/matrix.json`) also pins the old coarse mapping. Decide: port the split + composite rawStopReason (requires re-baselining the matrix fixture), or record as accepted coarse divergence. |
| B-5 | **Image/classifier model infrastructure**: `ModelType` (`chat`/`image`/`classifier`), `AnyModel`, `getAllModels`/`getModelsOfType`, catalog flattening across all providers, classifier APIs | `a328aa89a`, `a7d17e39a`, `89a5c7bda`, `04efdfc38` | Out of ADR 0059 chat-only scope | Confirm as Deferred (the current README defers "images" and non-scoped provider families). One scope call covers the whole cluster. |
| B-6 | **Extension observability + agent-side types**: `onProviderStreamEvent` option, `NestedToolCallRecord`/`toolResult.nestedCalls` (codemode), virtual-model types, `AssistantMessage.thinkingLevel` | `002fc8385`, `8562bcf66`, `540e174c7` | Out of scope (extension/coding-agent surfaces) | Confirm as Deferred. |

**B-4 status (2026-10-03): PORTED (issue #864).** `map_responses_termination` now splits `incomplete` on the provider reason: `max_output_tokens` stays `length`, any other reason (or none) is `error` with pi's message. The matrix fixture was re-baselined (`incomplete` now `error`, with `incomplete.max_output_tokens` → `length` and `incomplete.content_filter` → `error` rows). `ResponsesEventProcessor` records the composite `<status>.<reason>` raw stop reason and forwards the reason to the mapper. Anthropic and Completions adapters are untouched (the refinement is Responses-only upstream).

## UPSTREAM-DRIFT display-only

- **D-1:** provider display name `OpenAI Codex` → `OpenAI Codex (legacy)` (`providers/openai-codex.ts:10`, `02eed88fd`). C++ pins the old name at `src/ai/BuiltinProviders.cpp:332` and `BuiltinProvidersTest` pins it in tests. Trivially portable if the fixture catalog is ever regenerated at v1.0.0; cosmetic, no behavior.

## v1.0.0 delta — three commits on top of v0.99.2

`git log 005af57d8..a13d35a74 -- packages/ai/src` returns exactly three commits. Verdicts below; none changes the GAP/blue/D-1 counts above.

| Commit | Change | Verdict |
| --- | --- | --- |
| `bc2d8dc1c` drop mismatched item ids when replaying grammar tool calls | Rewrites the Responses history-replay tool-call id drop rule in `openai-responses-shared.ts:294-307`. Old rule dropped non-`fc_*` ids only when replaying `custom_tool_call` as `function_call`; new rule always drops an id whose prefix does not match the replayed item type (`fc_` for `function_call`, `ctc_` for `custom_tool_call`). The commit message states the old rule broke switching OpenAI models through Radius after a codemode call. | **OUT-OF-SCOPE (Deferred surface).** The C++ replay path only emits `function_call` items with `fc_`-normalized ids; `ctc_`-style custom tool ids and the codemode grammar-tool replay live behind the deferred grammar/custom-tool-call machinery. No behavioral delta on the scoped DeepSeek/OpenAI wire paths. One caveat, recorded as INCONCLUSIVE rather than ALIGNED: the new rule also drops `fc_*` ids when `isDifferentModel` is true, whereas the old rule only dropped them for cross-provider different-model messages. The scoped C++ path never replays across models, so this is unobservable today; no golden pins it. |
| `233f17440` use color Pi logo on OAuth pages | Changes `utils/oauth-page.ts` (embedded OAuth success/error page HTML). | **OUT-OF-SCOPE.** The C++ `oauth_success_html` / `oauth_error_html` bytes are frozen against the v0.87.1 baseline (`fixtures/pi-ai/auth/oauth-*.html`, byte-compared by `OpenAICodexOAuthTest`). The C++ pages intentionally keep the old monochrome logo; the color-logo refresh is a cosmetic upstream drift on a frozen presentation surface. |
| `7a11fe1c7` add copy code login method to Anthropic OAuth | Adds a copy-code login branch to `auth/oauth/anthropic.ts` (+62 lines). | **OUT-OF-SCOPE.** The `anthropic` provider is not one of the eight bundled providers; its OAuth flows are owned by the later pi-coding-agent gate per ADR 0032. |

Re-verification against v1.0.0 (`a13d35a74`) confirmed the load-bearing ChatGPT sign-in surface is unchanged from v0.99.2: `DYNAMIC_CLIENT_ID = "dynamic_agent_client"`, `DIRECT_TOKEN_SCOPE = "chatgpt.tokens.use.direct"`, the `urn:uuid:<deviceId>` agent-host id, and the `isChatGPTSignIn` request-shaping branch all sit at the same lines cited in B-1.

---

## Previously recorded divergences — re-audited, confirmed accurate

The following yellow rows in `fixtures/pi-ai/README.md` were re-checked against v0.99.2 and remain correctly classified:

- `originator: pike` / `User-Agent: pike` client identity, including the authorize-URL `originator=pike` (residual note 6). pi still sends `originator: pi` / `getPiUserAgent()` at v0.99.2.
- zstd SSE request-body compression omitted (C++ uses pi's plain-JSON branch).
- Kimi production catalog = hand-authored vendor oracle (`openai-completions` + API-key) vs upstream's Anthropic-shaped artifact (ADR 0059 exception; residual note 1). Upstream `kimi-coding.models.ts` at v0.99.2 is still Anthropic-shaped; the divergence is stable.
- Login prompt rejects empty submissions (residual note 8).
- Lazy-adapter module-loading shims have no C++ equivalent by design.

## Confirmed ALIGNED without drift (judge spot-check passed)

A sample of high-risk ALIGNED rows was re-opened and verified:

- `incomplete` → single `Length` mapping in `Termination.cpp` matches the frozen matrix (see B-4 for the upstream refinement).
- Unfinished-stream guard wording "OpenAI Responses stream ended before a terminal response event" is byte-identical (`ResponsesEventProcessor.cpp:285-288` vs pi `:761-762`).
- `applyMessagePhaseStopReason` (partials flip to `stop` at `final_answer`) and `rawStopReason` capture on Responses.
- Completions developer/system role selection via `compat.supports_developer_role` (`CompletionsPayload.cpp:392-396`), Kimi vendor thinking-level map and clamping.
- Anthropic string-content raw/blank-drop/cache-promotion and `cacheWrite1h` default-0 on `message_start`.
- Retry-After parsing: C++ `provider_backoff_hint_ms` falls through to exponential backoff on unparseable values, matching pi's post-`2bbfcca43` behavior (P4 initially flagged this as a GAP, then corrected itself; judge confirms C++ already matches).
- `calculateCost` tier selection and 1-hour 2× cache-write rule; `getSupportedThinkingLevels`/`clampThinkingLevel`; 4-level auth precedence; OAuth double-checked refresh under store lock; `logout` = local removal only.

## OUT-OF-SCOPE / Deferred (no action, recorded for completeness)

- All non-scoped adapters (mistral, gemini, claude-code, bedrock, azure, google, pi-messages, cloudflare, typesafe-system-one, llama-cpp-classify, openrouter-images) and their drift commits (`dc84c1ac0`, `8930b9ec0`, `3dd803d7e`, `33e203354`, `a7d17e39a`).
- `serviceTier` pricing (`a6ca86102`) — already on the Deferred list ("usage multiplier unreachable"), confirmed unchanged by this audit.
- **Sampling params merge removal** (`c01f687e5`) — moved from a decision to an observation. Upstream moved the `model.samplingParams` merge off the `streamSimple` path into direct `stream()`/`complete()`. The C++ surface is `streamSimple`-only (row 5) and never carried a `samplingParams` field, so there is nothing to drift. The residual behavioral nuance — a hypothetical upstream `streamSimple` caller that relied on model-default sampling params loses that merge — has no C++ counterpart to affect. Recorded here so the port-or-defer ruling is explicit rather than silent.
- Shared OAuth callback server refactor (`4df157433`) — behavior-preserving for the Codex path; C++ `OAuthCallbackServer` already implements the degraded-to-manual semantics.
- openai SDK 7.19.0 bump (`ab30693d6`) — type-level only.

## Coverage predicate

- 193/193 files under `packages/ai/src` @ v0.99.2 classified (A=103 audited, B=41 scope-call, C=49 deferred). The full A/B/C partition table is archived in the P5 worker transcript under the session's `agent-transcripts/` directory (agent `52696a3d-b8af-4379-9c36-84efd60e4ec6`, file `52696a3d-b8af-4379-9c36-84efd60e4ec6.jsonl`, "Partition table (grouped)" section).
- 21/21 commits in `f07218c4..005af57d8` triaged; the 21st (`a6ca86102`, initially missed by the main thread) was caught and triaged by P5.
- Every ALIGNED row carries a pi `file:line` @005af57d8, a C++ file, and a test→fixture pointer. Judge spot-checks re-opened a sample of each worker's citations; no fabricated citation was found; two worker claims were corrected during judging (P2's incomplete-mapping severity, P1's SSE-header nuance).

## What pike's maintainer should do next

1. ~~Decide GAP-1~~ **Done (2026-10-03):** ported as commit `105eae3ec`. ~~GAP-2~~ **Done (2026-10-03):** ported as issue #863, WS always sends with uuidv4 fallback, SSE keeps the conditional empty-set shape.
2. ~~Decide B-1 through B-6 as a batch~~ **In progress (2026-10-03):** B-1 and B-3 ported together as commit `3493fc8a9` (issue #862, ADR 0032 amended). B-4 ported as issue #864 (matrix fixture re-baselined). Remaining: B-2 (Anthropic federation, low urgency), B-5/B-6 (quick Deferred-list confirmations).
3. If the pi-ai gate re-baselines from `f07218c4` to v0.99.2, every remaining blue row (B-2, B-5, B-6) must be resolved first; both GAPs and B-1/B-3/B-4 are now ported. The fixtures README's "no partial placeholders" classification requires each blue row to land as either a new Supported row with evidence or a Deferred entry.
