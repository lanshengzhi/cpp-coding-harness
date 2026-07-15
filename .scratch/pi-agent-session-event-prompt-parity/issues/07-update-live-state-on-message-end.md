# 07 — Update live session state on message end

**What to build:** Let SDK hosts observe current conversation state from inside message-end callbacks by updating AgentSession history before subscribers receive each completed message.

**Blocked by:** 03 — Unify queued-message lifecycle events; 04 — Align tool execution events and tool-result messages.

**Status:** completed

- [x] Use PRD stories 4 and 20 and pi's state-first AgentSession event handling as the ordering authority.
- [x] Completed user, assistant, queued, and tool-result messages enter live history exactly once before subscribers run.
- [x] `message_count` and `last_assistant_text` queried inside a message-end subscriber reflect the just-completed message.
- [x] The completed agent-loop result cannot duplicate or roll back event-driven live state.
- [x] Tests drive prompts and subscriptions through public AgentSession behavior rather than private event-handler structure.
- [x] The focused AgentSession/SDK test slice passes.

**Completion evidence:**
- `AgentSessionRuntime::make_combined_sink` now appends `MessageEndEvent` messages to live history before forwarding events to subscribers, matching the pi state-first ordering.
- `AgentSessionRuntime::run_agent_loop` no longer assigns `session_.history = std::move(result->context.messages)`; it persists only the entries added since `previous_size` from the already-updated live history.
- Removed the double `make_combined_sink` wrapping in `run_prompt` so events are delivered exactly once.
- Added SDK tests proving that `message_count` and `last_assistant_text` inside a `MessageEndEvent` subscriber reflect the just-completed message, and that subscriber failures leave live state in place while the session remains usable.
- All tests pass: focused `[sdk][live-state]`, full `[sdk]`, `[agent][async]`, `[architecture]`, and the full `ctest` preset.
