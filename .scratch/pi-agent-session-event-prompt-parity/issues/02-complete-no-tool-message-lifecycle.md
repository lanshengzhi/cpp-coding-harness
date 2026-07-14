# 02 — Complete the no-tool message lifecycle

**What to build:** Make a normal prompt with no tool calls observable as one complete pi-aligned run: user message lifecycle, streamed assistant lifecycle, completed turn, and completed agent run.

**Blocked by:** 01 — Cut over the core AgentEvent value contract.

**Status:** ready-for-human

- [x] Use PRD stories 10, 12–14, and 17 plus the current pi no-tool `runAgentLoop` and assistant-stream flow as the behavioral authority.
- [x] A fake-provider prompt emits user `message_start` and `message_end` before the assistant response lifecycle.
- [x] Every assistant update carries both the current assistant message value and its provider-neutral assistant stream event.
- [x] `turn_end` carries the completed assistant message with an empty tool-result list, and `agent_end` carries all messages produced by the run.
- [x] Focused `[agent][async]` tests prove exact semantic order and payload meaning without depending on private helpers.
