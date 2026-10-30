# Tool duration comparison progress (#1106)

## Scope

Baseline pi-v1.3.0, revision `71023aec1546d6d677a65669c5665c0927ce0dd0`. This is a partial implementation record, not a whole-row receipt.

## Delivered behavior

The Agent measures execution before closing the update gate. Nested end events receive the execute duration from the host; the nested record separately measures the pipeline. A process-local realtime-clock interposer jumps CLOCK_REALTIME while leaving CLOCK_MONOTONIC unchanged. The Agent tool execution test observes both the jump and a normal duration. A nested call with a slow before hook distinguishes the end-event duration from the parent execution window.

The candidate `audit-1106` capture drives frozen `runToolCall` and records: cancellation before execution (no duration), cancellation during execution (duration present), after-hook failure (duration present), and an unknown tool (no duration). The candidate manifest passes `audit_evidence.py check`.

## Validation

Focused selection after review fixes: 4 CTest entries passed (clock jump, nested end-event mapping, nested slow-hook discriminator, nested cancellation). Full validation after review fixes selected 4,021 entries: 4,019 passed and 2 skipped (the clock fixture's ordinary entry skips while the interposed wrapper runs, and the Native TUI differential capture lacked its prerequisite). The architecture and warning gates passed as part of the full run; the separate architecture selection passed all 43 entries. The capture used the clean `../pi` checkout; its local esbuild executable was unavailable, so `ESBUILD_BINARY_PATH` selected the compatible executable from the frozen `/tmp/cch-1168-pi-frozen` checkout.

## Capability status

No capability row is promoted. The new evidence family is a candidate and is not registered.

## Remaining boundary

Registering the candidate changes `fixtures/pi-v1.3.0/baselines.json` and invalidates existing whole-file registry pins in `docs/research/pi-v1.3.0/1058-deliveries.json`. A trial registration caused the consolidation check to fail with a delivery digest mismatch; the registration was reverted. Existing receipts need a separately validated migration or rebind before this family can be registered. Matched frozen/C++ comparisons, exact update-gate contention evidence, and complete whole-row receipts remain open; #1106 must remain open.
