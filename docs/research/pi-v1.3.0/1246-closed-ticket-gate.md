# #1246 closed implementation-ticket gate, 2026-11-06

## Scope and measured change

A closed implementation ticket is a stale-field contradiction only when the
row's implementation is missing or partial and its verification is not verified.
A verified or implemented row can normally have a closed delivery ticket. This
change does not promote any capability.

At base `64a20eeb3572f05114de9c5fb3e13ceb14269e4c`, the offline baseline had
777 entries. Comparing each entry to the final consolidated ledger removes 567
exempt closed-ticket entries and leaves 210: 28 closed-ticket contradictions,
93 consumer entries and 89 gaps entries. The latter two classes retain the
previous gate and its documented limitations in
[1239-stale-field-gates.md](1239-stale-field-gates.md).

Thirteen delivery receipts pin `consolidate_audits.py`; no receipt pins the
baseline fixture. Each affected receipt records `revalidation1246`: the gate
change does not change its claimed rows, comparison scenarios, routing or
selected evidence. Their changed script digest was rebound; no capability claim
or comparison was added.

## Validation

The focused stale-field tests cover the two exemptions and the retained failure;
the verified promotion case uses closed ticket #1178, which blocked #1240.
Both exemption tests were observed failing before their respective gate changes.
`ConsolidationAuditTest.py` passed 93 tests, consolidation build and check passed,
`receipt_boundary.py` passed 32 receipts, 808 claims and 966 comparisons, and
`audit_evidence.py verify` checked 84 bundles, 180 artifacts and 1,012 scenarios.
Python compilation and the incremental build with the Parity Architecture Gate
passed. After the full run, the promotion test was refined to name the affected
#1240 chord row; the 15 stale-field tests passed again.

The full `TERM_PROGRAM=herdr CCH_LANE_NAME=lane-1246 ctest --preset vcpkg`
run selected 4,561 tests: 4,549 passed, 1 failed and 11 were skipped. The failure,
`Durable edit tool reproduces the frozen BOM CRLF diff and rejection results`,
reported a concurrent edit ordering mismatch (486 of 492 assertions passed).
The same case passed in isolation immediately afterward. A full run in the
[#1239 record](1239-stale-field-gates.md) reported the same case's concurrent
edit diff mismatch; this isolated pass does not establish the cause of either
failure. Full Validation therefore remains red.
