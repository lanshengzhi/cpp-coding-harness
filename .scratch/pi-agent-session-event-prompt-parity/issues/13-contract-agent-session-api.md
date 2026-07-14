# 13 — Contract AgentSession to one prompt and subscription path

**What to build:** Give SDK hosts one prompt completion contract—success or explicit error—and one persistent subscription path for progress, with resulting state queried separately.

**Blocked by:** 06 — Narrow AgentSession prompt interpretation; 09 — Recover from subscriber delivery failure; 10 — Recover from incremental persistence failure; 11 — Render text CLI output from AgentSession subscriptions; 12 — Emit direct pi-shaped JSON events.

**Status:** ready-for-agent

- [ ] Use PRD stories 1–3, 5, 8, 42–43, and 51 plus pi `AgentSession.prompt` and `subscribe` as the public-contract authority.
- [ ] Public prompt completion returns only success or an explicit C++ error; PromptResult and private status/result models are absent.
- [ ] Per-prompt event sinks, sink-composition policy, compatibility overloads, and duplicate event paths are absent.
- [ ] State accessors remain the only way to query message count and last assistant text after completion or failure.
- [ ] RPC is migrated mechanically to the persistent subscription path without pre-empting ticket 14's protocol-ordering work.
- [ ] SDK source-seam and architecture tests prove removed symbols stay absent and move-only subscriptions remain supported.
