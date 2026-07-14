# 04 — Align tool execution events and tool-result messages

**What to build:** Make tool activity reconstructible through pi-aligned tool execution values and ordinary tool-result message lifecycles, including deterministic result ordering and safe handling of truncated calls.

**Blocked by:** 02 — Complete the no-tool message lifecycle.

**Status:** completed

- [x] Use PRD stories 12, 15, and 18 plus the current pi tool execution and `emitToolResultMessage` behavior as the authority.
- [x] Tool execution start/end events carry the supported call id, name, arguments/result, and error meaning without numeric turn fields or flattened content substitutions.
- [x] Every finalized tool result emits ordinary `message_start` and `message_end` before it appears in `turn_end`.
- [x] Multiple tool results remain in assistant source order even when execution completion order differs.
- [x] Length-truncated tool calls are not executed and still produce aligned error results and message lifecycles.
- [x] Focused `[agent][async]` and `[agent][tool-executor]` tests pass; unsupported tool progress events are not introduced.

**Completion evidence:**
- Removed `turn` from `ToolCallBatchRequest`.
- `ToolCallExecutor` emits `message_start`/`message_end` for every finalized tool result, with parallel results emitted in assistant source order.
- `AgentLoop` emits the same message lifecycle for length-truncated tool calls without executing them.
- All 587 tests pass; `[agent][async]` and `[agent][tool-executor]` slices pass.
