# #1240 remaining Chord discriminators

## Scope and owner decision

This follows the [strict-JSON pilot](1240-negative-control-pilot.md), which
promoted `cch_chord:json:strict-validation`. On 2026-11-07 the owner authorized
extending comparison reuse within #1240: a claim may reuse its comparison when
an individually reviewed, passing negative control exercises the same
registered frozen scenario, caller actions and acceptance judgment, and rejects
a named shallow implementation. A passing test or validator alone is not
sufficient. Claims lacking that evidence stay bounded; this decision does not
infer that claims outside #1240 qualify.

The controls model particular defects; they do not prove correctness against
every possible implementation. The masked-context ordering case remains a C++
regression rather than a frozen discriminator.

## Delivered behavior and capability status

The following ten rows are promoted in addition to the strict-validation
pilot, for eleven of the original fifteen claims. Each reused comparison names
a distinct, pinned, passing negative control:

| Row | Frozen judgment and rejected shallow implementation |
| --- | --- |
| `cch_chord:context:without-abort-signal` | Pass-through retains the cancelled parent; a fresh context drops its typed value. Both fail the same masked-state judgment. |
| `cch_chord:delta:batch-application` | Applying only the first frozen batch fails the final revision while preserving the base. |
| `cch_chord:delta:immutable-application` | A deep copy has the expected value but loses the captured retained-container identity. |
| `cch_chord:delta:mutable-application` | Whole-batch decoding returns the captured empty-path error before the captured leading write. |
| `cch_chord:delta:prepare-replace` | A preparation that ignores abort permits adoption and fails the captured error, value and revision. |
| `cch_chord:state:attached-hydration` | A shallow state that ignores buffered frames from a conforming source retains its snapshot; the complete frozen delivery and final-value judgment rejects it. This does not independently establish every intermediate frame's order. |
| `cch_chord:state:dispose` | A coupled attachment lifecycle fails the captured count when the first attachment is disposed twice. |
| `cch_chord:Chord:draft` | A mutable overlay that survives settlement accepts the escaped write instead of reporting the captured error. |
| `cch_chord:json:copy-json` | A copy that aliases its source changes that source when the captured copy mutation is applied. |
| `cch_chord:json:json-value` | A NaN-only predicate accepts root positive infinity, whose named frozen input is defined by the [capture script](../../../fixtures/pi-v1.3.0/audit-1057-typed-json/capture-1057-typed-json.mts). |

## Remaining boundary

- `cch_chord:delta:overlap`: the frozen scenario tests a zero cap, but no
  frozen observation establishes a positive cap that truncates a matching
  overlap. The zero-cap test remains a regression, not whole-row evidence.
- `cch_chord:Chord:json`: the frozen copy-mutation observation exists, but
  neither the `to_support()` mutation case nor the attempted source-mutation
  control establishes a valid discriminator under the same captured actions.
  Admission detachment and immutable-copy sharing remain bounded.
- `cch_chord:state:listener-error`: the frozen late-rejection observation exists
  and the new control rejects error suppression, but the shared comparison
  checks messages rather than the captured error name and identity. The C++
  `support::Error` value carrier does not represent JavaScript object identity.
- `cch_chord:state:delivery-order`: existing registered observations and
  comparisons cover held and serialized delivery, but this slice established
  no separately reviewed executable negative control for the full claim.

## Evidence and review

Five test sources changed. The four Chord receipts #1178, #1185, #1186 and
#1188 pin those sources; #1184 additionally pins the context source. Each
receipt retains its prior validation under `revalidation1240Remaining`, updates
only its affected test-source digests and records the selection's JUnit file.
The #1184 selection does not include the new masked-context control; that
control is in the separate Chord selection.

Independent spec review identified and prompted corrections to the draft,
replacement, disposal, batch and listener-error controls and rejected the
`Chord:json` promotion. Follow-up review accepted the revised batch, disposal,
draft and hydration controls for their specified defects; listener-error
remains bounded for the narrower message judgment. The nested NaN regression is
separate from the frozen root-infinity result. Standards review identified
unchecked numeric conversions, throwing filesystem overloads and callback
lifetime documentation; these were corrected. A final standards recheck found
one missing lifetime comment, which was added.

## Validation

- Full incremental build passed; Chord selection **104/104** and #1184
  selection **10/10** passed with committed JUnit artifacts.
- Consolidation build/check passed with 577 delivery-backed rows; the evidence
  suite passed 94 tests. `receipt_boundary.py` passed for 32 receipts, 808
  claims and 966 comparisons. Frozen evidence verification passed for 84
  bundles, 180 artifacts and 1,012 scenarios.
- `scripts/format-check.sh fc062f2de` and `git diff --check HEAD` passed.
- Final unfiltered `CCH_LANE_NAME=lane-1240-a TERM_PROGRAM=herdr ctest --preset
  vcpkg` selected **4,575**, passed **4,564**, and skipped the 11 documented
  environment-dependent cases in 261.6 s.

Two earlier full-run failures in this session are recorded rather than
explained away. `cch_warning_gate` failed because of the missing field
initializers this slice added; the fix landed before the runs above and the gate
now passes there. The first final-form full run failed one test outside this
slice's files: `Durable edit tool reproduces the frozen BOM CRLF diff and
rejection results` (`tests/durable/EditToolTest.cpp:293`/`:297`) compared two
concurrent tool results index-wise against a call-id-ordered frozen array and
reproduced **45 times in 200 isolated runs**; the lossless file outcome still
passed, so the defect is the test's settlement-order assumption. It is filed as
#1247, the test file is unchanged from `origin/main`, and the rerun above is
green. The earlier `bounded parallel cancellation leaves queued tool
capabilities unstarted` (#2062) failure did not reproduce here: it passed in
both full runs.

Before this slice, an unfiltered full run passed 4,563 selected tests, including
the previously failing TUI test. That earlier TUI failure did not reproduce in
100 serial repetitions or 90 parallel repetitions; its cause remains unknown.
