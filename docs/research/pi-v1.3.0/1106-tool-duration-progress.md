# #1106 tool duration delivery

## Scope

Baseline pi-v1.3.0, revision `71023aec1546d6d677a65669c5665c0927ce0dd0`. This delivery records the bounded behavior, discriminators, and validation for #1106.

## Delivered behavior

The Agent measures execution before closing the update gate. Nested end events receive the execute duration from the host; the nested record separately measures the pipeline. A process-local realtime-clock interposer jumps CLOCK_REALTIME while leaving CLOCK_MONOTONIC unchanged. The Agent tool execution test observes both the jump and a normal duration. A nested call with a slow before hook distinguishes the end-event duration from the parent execution window.

An after-hook failure preserves the tool execution duration on the result and the end event. A tool call cancelled before execution begins (or unexecuted due to length truncation or unknown name) records no duration.

The candidate `audit-1106` capture drives frozen `runToolCall` and records: cancellation before execution (no duration), cancellation during execution (duration present), after-hook failure (duration present), and an unknown tool (no duration). The candidate manifest passes `audit_evidence.py check`.

## Validation

- Focused selection: 4 CTest entries passed (clock jump `RealtimeJumpTest.py`, nested end-event mapping, nested slow-hook discriminator, nested cancellation).
- `ToolCallExecutor` selection: 8/8 passed.
- Full validation: 4,021 selected, 4,019 passed, 2 skipped (the clock fixture's ordinary entry skips while the interposed wrapper runs, and the Native TUI differential capture lacked its prerequisite).
- Architecture and warning gates passed as part of the full run; the separate architecture selection passed all 43 entries.
- Format check clean against `main` merge base.
- Capture used the clean `../pi` checkout at `71023aec...`.

## Capability status

Bounded slice implementation and discriminators delivered. Whole-row capability ledger promotion and delivery receipt re-pin are owned by #1176 (Agent verification) and #1183 (Consolidation index).

## Remaining boundary

Registration of `audit-1106` into `baselines.json` and delivery receipt re-pin are assigned to #1176/#1183 to avoid uncoordinated pin invalidation across tickets. Model-response timing remains excluded per ADR 0075. #1044 remains open.

