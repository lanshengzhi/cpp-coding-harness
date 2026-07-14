# 08 — Persist completed messages incrementally

**What to build:** Make durable session history advance one completed message at a time after subscribers have observed it, instead of waiting for the entire agent run to finish.

**Blocked by:** 07 — Update live session state on message end.

**Status:** ready-for-agent

- [ ] Use PRD stories 21–22 and 24–25 and pi's message-end append behavior as the persistence authority.
- [ ] Supported user, assistant, and tool-result messages append only on `message_end` and only after subscriber delivery completes.
- [ ] The post-loop history-delta append pass and any completed-turn commit, staging, rollback, or poison policy are absent.
- [ ] Successful long/tool-using runs become durable incrementally and reopen with the same durable message order.
- [ ] Existing redaction, private permissions, symlink rejection, append-only format, and bounded tool-output protections remain intact.
- [ ] Focused SDK, session lifecycle, and `[harness][session]` tests pass.
