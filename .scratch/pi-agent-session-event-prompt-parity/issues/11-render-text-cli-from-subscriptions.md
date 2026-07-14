# 11 — Render text CLI output from AgentSession subscriptions

**What to build:** Make text mode present agent progress and responses from the same persistent AgentSession event stream used by SDK hosts, with frontend command outcomes handled separately.

**Blocked by:** 04 — Align tool execution events and tool-result messages; 05 — Move text CLI built-ins to the CLI adapter; 08 — Persist completed messages incrementally.

**Status:** ready-for-agent

- [ ] Use PRD stories 34–36 and 40 as the text adapter authority.
- [ ] Text mode subscribes once and renders supported AgentSession events without a per-prompt event sink.
- [ ] Assistant text and tool activity are not duplicated by separate prompt-result presentation.
- [ ] Frontend command feedback remains text-mode behavior, while unmatched slash input reaches AgentSession.
- [ ] Prompt failures are presented without reviving PromptResult status codes as a second lifecycle model.
- [ ] Process-level text CLI tests and the focused `[cli]` slice pass with fake providers only.
