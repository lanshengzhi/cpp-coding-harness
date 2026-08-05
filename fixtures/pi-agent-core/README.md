# pi-agent-core compatibility fixtures

The committed evidence bundle for the pi-agent-core completion gate ([#330]/[#331], spec
[#349]). This directory mirrors the `fixtures/pi-ai/` strategy: every fixture is compared by
tests in this repository against the C++ surface, so the gate's evidence is one checklist away.
No fixture value is a live credential or derived from one; all strings are distinguishable
dummy values (see [Sanitization rules](#sanitization-rules)).

This file records only the capabilities landed so far (T01 [#350], T02 [#351]); the capability
checklist grows with each subsequent ticket (T03–T14, blockers-first per parity map [#2]), and
rows below cover only what this ticket touches.

## Pinned baseline

- **Frozen pi commit:** `83114817c68f5413e4d7ba6d7003ddc511cd31d2` (the parity map [#2]
  baseline). The local pi checkout is `../pi`; `pi:` references resolve from that root.
- **Published artifact:** `@earendil-works/pi-agent-core@0.83.0`, published at pi tag
  `v0.83.0`. The harness consumer pinned here is `packages/agent/src/harness/agent-harness.ts`
  (`createStreamFn`) with the loop order of `packages/agent/src/agent-loop.ts` (`runLoop`).

## Sanitization rules

- Every credential-like string in these fixtures is a distinguishable `dummy-*` token; the
  model/session values are `gpt-test`, `session-golden`, and header values like `value`.
- No fixture value was copied from a live service, a real `auth.json`, or a real `models.json`.
- Live `~/.pi/agent/auth.json` and `~/.pi/agent/models.json` are compatibility/evidence inputs
  and must never be pasted into fixtures, logs, issues, or commits (parity map [#2]).

## Fixture inventory

### Option-forwarding goldens (`stream-simple-options-*.json`)

The per-turn `streamSimple` option set as the C++ Agent forwards it, captured through the
recording fake `ModelRuntime` (`tests/support/FakeModelRuntime.hpp`) and named exactly like pi's
harness-consumer set (`agent-harness.ts` `createStreamFn`): `reasoning`, `sessionId`,
`cacheRetention`, `timeoutMs`, `maxRetries`, `maxRetryDelayMs`, `headers`, `signal`. The model
identity is included because the fake records it with the call.

- `stream-simple-options.json` (#351): the full host-configured set — `reasoning: "high"`,
  `sessionId`, `cacheRetention` resolved `"short"`, `timeoutMs`, `maxRetries`, `maxRetryDelayMs`,
  one `headers` entry, and the active prompt signal.
- `stream-simple-options-default.json` (#351): every knob at its default — `off` thinking
  forwards no `reasoning` (pi `createLoopConfig` `off → undefined`), no session id, no timeout or
  retry overrides, empty headers, `cacheRetention` still resolving to the pi default `"short"`
  (unset → `Short` via `resolve_cache_retention`, ADR 0033), and the signal present.

`transport` stays fixed per adapter (the C++ `SimpleStreamOptions` has no transport member, per
#329) and `metadata`/`onPayload`/`onResponse`/`thinkingBudgets` are absent with no placeholder.

### Loop lifecycle goldens (`loop-*.json`)

The ordered `AgentLifecycleEvent` stream (plus per-call forwarded options where they prove a
hook effect), serialized canonically and byte-compared.

- `loop-lifecycle.json` (#351): one rich run covering `agent_start` → `turn_start` →
  `message_start/update/end` → `tool_execution_*` → `turn_end` → `agent_end`, with the
  steer drain point (a steering message steered from the `turn_end` observer drains at the end
  of turn 1 and injects before the turn-2 request), the follow-up drain point (a follow-up
  drains after the last tool-call turn and runs as turn 3), prepare-next-turn (its first update
  flips the thinking level `medium → high`, visible in the recorded `reasoning` of stream calls
  2/3), tool-call continuation (turn 1 → 2) and follow-up continuation (turn 2 → 3).
- `loop-terminal-error.json` (#351): the `error` terminal ordering — synthesized assistant
  start from the authoritative final message (the #326 terminal-before-start recovery), exactly
  one assistant lifecycle, `turn_end` with `stopReason: "error"`, then `agent_end`.
- `loop-terminal-aborted.json` (#351): the `aborted` terminal ordering under cancellation
  (ADR 0020) — same shape with `stopReason: "aborted"`.

### Terminal matrix

The six-category terminal matrix (`model_source`, `model_validation`, `provider`, `stream`,
`auth`, `oauth`) is a test matrix in `ModelRuntimeSeamTest` (`[issue351]`), not a fixture: each
row scripts a terminal failure of one category through the fake runtime and asserts exactly one
terminal event plus an agreeing final `AssistantMessage`, with the category flowing through the
single `util::Expected` error value (the #326 six-category channel).

## Capability-to-source checklist

One line per scoped capability, tying it to the frozen pi source, the resolution record, the C++
surface, and the committed evidence. Resolution records: [#326]
`7d813af3650dfa4fd098e90e321fce24`, [#329] `746839885c04cf195984af7112f2ea88`, [#330]
`6d06d3172ff3383ed3188a9bef4be587`, [#331] `3a9243c1cb711cecbde25396a5af53cd`.

### Supported Capabilities (this ticket's scope)

| # | Capability | Frozen pi source | C++ surface | Evidence (tests → fixtures) |
| --- | --- | --- | --- | --- |
| 1 | Per-turn harness-consumer option set through `streamSimple`: `reasoning` (thinking level, `off` → undefined), `sessionId`, `cacheRetention` (default `"short"`), `timeoutMs`, `maxRetries`, `maxRetryDelayMs`, `headers`, `signal` | `packages/agent/src/harness/agent-harness.ts` `createStreamFn`; `packages/agent/src/agent.ts` `createLoopConfig` (`thinkingLevel === "off" ? undefined : thinkingLevel`) | `include/cch/agent/AgentContext.hpp` (`AsyncAgentOptions`), `src/agent/AgentLoop.cpp` (per-turn forwarding) | `ModelRuntimeSeamTest` `"Agent forwards the full harness-consumer option set…"` + `"default turns forward pi's harness-consumer defaults…"`; `AgentCoreEvidenceTest` → `stream-simple-options.json`, `stream-simple-options-default.json` |
| 2 | `transport` fixed per adapter; `metadata`/`onPayload`/`onResponse`/`thinkingBudgets` omitted with no placeholder | `agent-harness.ts` `createStreamFn` (options 2/3/4/5 per [#329]) | `include/cch/ai/RequestOptions.hpp` (`SimpleStreamOptions` has no such members — architecture-pinned) | `ArchitectureSurfaceScanTest` `"streamSimple exposes only the supported caller option set"` |
| 3 | Terminal-error-event contract: exactly one `error`/`aborted` terminal event plus a final `AssistantMessage` with agreeing `stopReason`/`errorMessage` (incl. terminal-before-start synthesized start) | #326; `packages/agent/src/agent-loop.ts` `streamAssistantResponse` (`done`/`error` → `message_start` synthesis) | `src/agent/AgentLoop.cpp` (synthesize-start + terminal turn), `tests/support/FakeModelRuntime.hpp` (exactly-one-terminal script) | `ModelRuntimeSeamTest` terminal/aborted/matrix rows; `AgentCoreEvidenceTest` → `loop-terminal-error.json`, `loop-terminal-aborted.json` |
| 4 | Six-category channel (`model_source`, `model_validation`, `provider`, `stream`, `auth`, `oauth`) through the single `util::Expected` error value; no second exception hierarchy | #326 | `include/cch/util/Error.hpp` (`ErrorCode` six categories), `tests/support/FakeModelRuntime.hpp` (`terminal_failure_code`/`last_terminal_failure`) | `ModelRuntimeSeamTest` `"six-category terminal matrix yields exactly one terminal event plus a final AssistantMessage each"` |
| 5 | Loop lifecycle order: `agent_start` → `turn_start` → `message_start/update/end` → `tool_execution_*` → `turn_end` → prepare-next-turn → stop-after-turn → steering → follow-up → `agent_end`; tool-call and follow-up continuation; `agent_end.messages` invocation-local | ADR 0014; `packages/agent/src/agent-loop.ts` `runLoop` | `src/agent/AgentLoop.cpp` | `AgentCoreEvidenceTest` → `loop-lifecycle.json`, `loop-terminal-error.json`, `loop-terminal-aborted.json`; existing `AsyncAgentLoopTest`/`AgentTest` ordering suites |

### Recorded divergences preserved (unchanged by this ticket)

- Bounded steer/follow-up queues remain the recorded ADR 0022 divergence (pi unbounded).
- The six-category terminal payload stays a recorded #326 C++ enrichment.
- No default turn limit stays ADR 0015 (explicit `max_turns` caps only).
- `transport` is fixed per adapter instead of a per-request option (the codex adapter is
  WebSocket-first with narrow SSE fallback, the Responses/Anthropic family plain SSE), matching
  the pi-ai wire goldens and [#329].

## Gate notes

- The fake `ModelRuntime` is the only seam used here (no live keys, no network validation);
  scripted responses and terminal categories drive every golden deterministically.
- E2E (agent-harness-through-`streamSimple`, TUI/CLI full-chain acceptance) belongs to the
  pi-tui / pi-coding-agent gates; this gate covers harness-level unit evidence.

[#2]: https://github.com/lanshengzhi/cpp-coding-harness/issues/2
[#326]: https://github.com/lanshengzhi/cpp-coding-harness/issues/326
[#329]: https://github.com/lanshengzhi/cpp-coding-harness/issues/329
[#330]: https://github.com/lanshengzhi/cpp-coding-harness/issues/330
[#331]: https://github.com/lanshengzhi/cpp-coding-harness/issues/331
[#349]: https://github.com/lanshengzhi/cpp-coding-harness/issues/349
[#350]: https://github.com/lanshengzhi/cpp-coding-harness/issues/350
[#351]: https://github.com/lanshengzhi/cpp-coding-harness/issues/351
