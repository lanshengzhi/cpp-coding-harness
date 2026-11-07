# #1240 strict JSON negative-control pilot

## Scope and owner decision

The owner authorized a single-row pilot of a reused comparison with an executable
negative control; the decision is recorded in [#1240](https://github.com/lanshengzhi/cpp-coding-harness/issues/1240#issuecomment-6703651162).
This pilot concerns only `cch_chord:json:strict-validation`. It does not settle
whether other bounded claims qualify or change the remaining ticket's promotion floor.

## Evidence

The existing comparison and its negative control load the registered
`audit-1057-typed-json/chord-typed-json.json` scenario `copyJson rejects a
non-finite number nested below the root`. Both use the same input builder and
failure judgment. The control implements a root-only copy: it rejects a root
NaN but accepts the nested NaN. The judgment accepts the production result and
rejects the shallow result, demonstrating the relevant distinction. This is a
model of a specific defect, not a proof against every shallow implementation.

The consolidation validator requires a separate passing, pinned negative-control
test and a nonempty reason when an unbounded claim reuses its comparison test.
It checks references, not test semantics; the latter remain a review obligation.
The existing comparison reads and checks both required registered scenarios.

## Evidence pin impact

The consolidation script is pinned by 13 receipts. Four of those also pin
`tests/chord/StrictJsonTest.cpp`: #1178, #1185, #1186 and #1188. Their Chord
selection ran 92 cases including the new control. The remaining nine receipts'
previous selections were rerun. Each receipt records its prior validation and
changed inputs in `revalidation1240Pilot`; JUnit artifacts record the new runs.
The strict-validation boundary is cleared only on #1186's reviewed claim.

## Validation

The focused comparison and control passed, and the Chord selection passed 92/92.
All 13 affected receipt selections passed. Consolidation build/check and the
receipt boundary check passed; the latter checked 32 receipts and 808 claims.
The consolidation check reports 567 delivery-backed rows, one more than before.
The first unfiltered full run selected 4,563 cases and failed two: the quarantined
projection performance case measured 2.267 ms against its 2 ms bound, and an MCP
server-exit case unexpectedly succeeded. Both passed when rerun individually in
a single CTest invocation. A second unfiltered run selected 4,563 cases: the
projection case passed, 11 cases were skipped, and the same MCP server-exit case
failed again. Full validation is therefore not green. The failure was an
unexpected successful call at `McpFailureIsolationTest.cpp:351`; its cause was
not established by this pilot.

Frozen-evidence verification passed 84 bundles, 180 artifacts and 1,012 scenarios.
Format and whitespace checks passed. Independent subagent review was unavailable;
the evidence and standards were reviewed locally.
