# 02 — Complete the no-tool message lifecycle

**What to build:** Make a normal prompt with no tool calls observable as one complete pi-aligned run: user message lifecycle, streamed assistant lifecycle, completed turn, and completed agent run.

**Blocked by:** 01 — Cut over the core AgentEvent value contract.

**Status:** ready-for-agent

- [ ] Use PRD stories 10, 12–14, and 17 plus the current pi no-tool `runAgentLoop` and assistant-stream flow as the behavioral authority.
- [ ] A fake-provider prompt emits user `message_start` and `message_end` before the assistant response lifecycle.
- [ ] Every assistant update carries both the current assistant message value and its provider-neutral assistant stream event.
- [ ] `turn_end` carries the completed assistant message with an empty tool-result list, and `agent_end` carries all messages produced by the run.
- [ ] Focused `[agent][async]` tests prove exact semantic order and payload meaning without depending on private helpers.
