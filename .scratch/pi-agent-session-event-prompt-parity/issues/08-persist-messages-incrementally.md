# 08 — Persist completed messages incrementally

**What to build:** Make durable session history advance one completed message at a time after subscribers have observed it, instead of waiting for the entire agent run to finish.

**Blocked by:** 07 — Update live session state on message end.

**Status:** implemented

- [x] Use PRD stories 21–22 and 24–25 and pi's message-end append behavior as the persistence authority.
- [x] Supported user, assistant, and tool-result messages append only on `message_end` and only after subscriber delivery completes.
- [x] The post-loop history-delta append pass and any completed-turn commit, staging, rollback, or poison policy are absent.
- [x] Successful long/tool-using runs become durable incrementally and reopen with the same durable message order.
- [x] Existing redaction, private permissions, symlink rejection, append-only format, and bounded tool-output protections remain intact.
- [x] Focused SDK, session lifecycle, and `[harness][session]` tests pass.

**Completion evidence:**
- `AgentSessionRuntime::make_combined_sink` now appends supported user, assistant, and tool-result messages after persistent and per-prompt subscribers accept each `MessageEndEvent`.
- Removed the post-loop history-delta append pass; durable history advances one completed message at a time without transaction, rollback, staging, or poison state.
- Added an SDK test proving each subscriber sees the current message in live history before it is durable, a tool-using run persists user/assistant/tool-result messages in event order, and reopening restores that durable order.
- Preserved the persistence-specific prompt error classification and updated SDK state-accessor documentation to describe live rather than durable history.
- Focused `[sdk]`, `[coding-agent][runtime][session]`, `[harness][session]`, `[architecture]`, and the full `ctest --preset system` suite pass.
