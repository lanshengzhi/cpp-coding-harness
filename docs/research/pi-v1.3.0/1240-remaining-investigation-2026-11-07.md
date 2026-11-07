# #1240 remaining work: investigation, 2026-11-07

> **Superseded.** This is the dated snapshot from the first half of the
> session; the owner's floor was met later the same day. The promoted rows,
> the remaining boundaries and the review outcome are in
> [1240-remaining-discriminators.md](1240-remaining-discriminators.md). Read
> this file for the investigation trail, not for current status.

## Scope and status

The strict-JSON pilot remains the only one of the original 15 claims promoted.
The owner's floor of eight remains unmet. No other claim has been promoted in
this investigation; candidate frozen scenarios still need a reviewed executable
discriminator and receipt revalidation.

## MCP full-suite failure

The pilot's full runs failed the test that expected a call after `debug/exit` to
always fail. The isolated case reproduced the same unexpected success using
`ctest --preset vcpkg -R '^an MCP call errors explicitly when the server dies between tools/list and tools/call$' --repeat until-fail:100 --output-on-failure`.
The client reconnects a dead stdio server on the next call, as required by
ADR 0066 and already covered by `McpServerManagementTest.cpp`. Depending on
whether the client has observed EOF, the call can either hit the dying process
or reconnect to a new server. A successful reconnect is not a transport failure.

The failure-isolation test now starts a private copy of the fixture and removes
that copy before requesting exit. A subsequent call cannot succeed by launching
a replacement; it must report a process error if the old connection fails or
reconnection fails. The separate server-management test retains the successful
reconnect case. The focused test passed 100 consecutive runs, and the existing
successful-reconnect case passed. This changes test setup only, not the client
or the registered frozen evidence.

An unfiltered full build passed. Full CTest selected 4,563 cases and failed one:
`Native TUI fullscreen scroll-to-end indicator paints on a direct-fullscreen
launch while the transcript is scrolled`, at `TuiModeSwitchTest.cpp:1756`,
where the actual row lacked the expected right border. The MCP test passed in
that run. The TUI case passed 25 isolated repetitions and another 15 repetitions
with three CPU load processes; its cause remains unestablished and the full gate
is not green. The full log is `/tmp/cch-1240-remaining-full.log` on this host.

Consolidation check, the 94-case consolidation evidence suite, receipt boundary
(32 receipts, 808 claims), and frozen evidence verification (84 bundles, 180
artifacts, 1,012 scenarios) passed. The first direct evidence-suite invocation
omitted `CCH_LANE_NAME` and failed the lane-lock gate; the correctly identified
rerun passed. Format and whitespace checks passed. Standards and specification
were reviewed locally; independent subagents were unavailable, so the requested
independent review remains outstanding.

## Evidence observations for the remaining claims

- The `delta:overlap` comparison reads three frozen cases. They include a
  zero search cap but no positive cap that truncates an otherwise matching
  overlap. A control based on that artifact can establish the zero-cap behavior;
  it cannot claim a measured positive-cap boundary from these inputs.
- The `json:json-value` artifact records an `infinity` result but its input
  records only case names, not a nested non-finite value. The comparison's
  constructed nested NaN is not itself an additional frozen input observation.
  Review the intended acceptance judgment before citing it as a control.
- `delta:batch-application` has frozen base, two ordered batches, base-after,
  and final result. A first-batch-only implementation is a plausible control,
  but it has not yet been implemented or checked against the comparison's
  exact judgment.
- `Chord:json` compares frozen copy detachment alongside C++ immutable-container
  sharing. The sharing assertion must not be represented as an independently
  recorded pi identity observation without supporting frozen evidence.

The regression `test_1239_bounded_claims_have_followups_and_reverted_rows` in
`tests/evidence/ConsolidationAuditTest.py` currently expects all 14 other rows
to remain bounded. Update its expectations for each independently reviewed
promotion; leaving it unchanged makes the evidence suite reject that promotion.

These observations do not exhaust the other claims or establish that any
candidate qualifies for promotion. The four receipts that pin the affected
Chord test sources require the measured revalidation described in the pilot;
`ChordContextTest.cpp` also affects #1184. Do not bulk-rehash them.
