# 13 — Contract AgentSession to one prompt and subscription path

Category: enhancement
**What to build:** Give SDK hosts one prompt completion contract—success or explicit error—and one persistent subscription path for progress, with resulting state queried separately.

**Blocked by:** 06 — Narrow AgentSession prompt interpretation; 09 — Recover from subscriber delivery failure; 10 — Recover from incremental persistence failure; 11 — Render text CLI output from AgentSession subscriptions; 12 — Emit direct pi-shaped JSON events.

**Status:** implemented

- [x] Use spec stories 1–3, 5, 8, 42–43, and 51 plus pi `AgentSession.prompt` and `subscribe` as the public-contract authority.
- [x] Public prompt completion returns only success or an explicit C++ error; PromptResult and private status/result models are absent.
- [x] Per-prompt event sinks, sink-composition policy, compatibility overloads, and duplicate event paths are absent.
- [x] State accessors remain the only way to query message count and last assistant text after completion or failure.
- [x] RPC is migrated mechanically to the persistent subscription path without pre-empting ticket 14's protocol-ordering work.
- [x] SDK source-seam and architecture tests prove removed symbols stay absent and move-only subscriptions remain supported.

## Comments

### Implementation

- `AgentSession::prompt` now returns `util::ExpectedVoid`; prompt progress is delivered only through persistent subscriptions, and resulting message count or assistant text is queried from session state.
- `AgentSessionRuntime` now propagates agent, subscriber, and persistence errors directly and no longer owns a private prompt status/result model or per-prompt sink composition.
- RPC subscribes once for the lifetime of the command loop while retaining its existing response/terminal ordering for ticket 14.
- SDK, runtime, CLI, RPC, architecture, and documentation call sites were migrated without compatibility aliases or overloads.

### Validation

- Red: the SDK source-contract assertions failed against `Expected<PromptResult>` and the event-sink-bearing `PromptOptions` before implementation.
- Build: `make -C build cpp_harness_tests -j2`.
- Focused: `[sdk]`, `[coding-agent][runtime][rpc]`, `[coding-agent][runtime][session]`, `[cli]`, and `[architecture]` all pass.
- Full: `ctest --preset system --output-on-failure` passes (1/1 CTest target; complete executable suite).
- Live provider validation was skipped because this contract migration is covered by fake/local tests and does not require network credentials.
