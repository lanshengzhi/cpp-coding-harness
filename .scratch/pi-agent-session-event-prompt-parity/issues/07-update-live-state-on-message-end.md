# 07 — Update live session state on message end

**What to build:** Let SDK hosts observe current conversation state from inside message-end callbacks by updating AgentSession history before subscribers receive each completed message.

**Blocked by:** 03 — Unify queued-message lifecycle events; 04 — Align tool execution events and tool-result messages.

**Status:** ready-for-agent

- [ ] Use PRD stories 4 and 20 and pi's state-first AgentSession event handling as the ordering authority.
- [ ] Completed user, assistant, queued, and tool-result messages enter live history exactly once before subscribers run.
- [ ] `message_count` and `last_assistant_text` queried inside a message-end subscriber reflect the just-completed message.
- [ ] The completed agent-loop result cannot duplicate or roll back event-driven live state.
- [ ] Tests drive prompts and subscriptions through public AgentSession behavior rather than private event-handler structure.
- [ ] The focused AgentSession/SDK test slice passes.
