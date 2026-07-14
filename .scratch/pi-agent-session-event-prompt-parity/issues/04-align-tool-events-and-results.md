# 04 — Align tool execution events and tool-result messages

**What to build:** Make tool activity reconstructible through pi-aligned tool execution values and ordinary tool-result message lifecycles, including deterministic result ordering and safe handling of truncated calls.

**Blocked by:** 02 — Complete the no-tool message lifecycle.

**Status:** ready-for-agent

- [ ] Use PRD stories 12, 15, and 18 plus the current pi tool execution and `emitToolResultMessage` behavior as the authority.
- [ ] Tool execution start/end events carry the supported call id, name, arguments/result, and error meaning without numeric turn fields or flattened content substitutions.
- [ ] Every finalized tool result emits ordinary `message_start` and `message_end` before it appears in `turn_end`.
- [ ] Multiple tool results remain in assistant source order even when execution completion order differs.
- [ ] Length-truncated tool calls are not executed and still produce aligned error results and message lifecycles.
- [ ] Focused `[agent][async]` and `[agent][tool-executor]` tests pass; unsupported tool progress events are not introduced.
