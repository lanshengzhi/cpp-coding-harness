# #1213 B5 alternate-screen overlay, wheel and mouse routing recaptured at pi-v1.3.0

## Scope

Route batch B5 of 17 (`docs/research/pi-v1.3.0/1182-tui-route-batches.json`,
disposition `comparison`). Recapture the historical pi-v1.0.4 mouse, wheel
and scroll-view observations at the frozen pi-v1.3.0 baseline
(`71023aec1546d6d677a65669c5665c0927ce0dd0`) and compare the C++ rows through
the named caller. 21 of 23 batch rows are receipted whole-row; 2 rows stay
unverified with the named blocker below.

## Delivered behavior

- The `audit-1213` family captures the v1.3.0 behavior of the mouse and
  scroll machinery through the frozen checkout's own modules, captured first
  in the batch sequence so the #1211/#1212 batches could reference it: the
  pointer report grammar (`mouse.json`: cached-frame hit test, capture drag
  with host focus, consumed/unconsumed wheel, motion and click counts,
  regular-screen terminal-owned mouse) and the scroll-view algebra
  (`scroll-view.json`: wheel accelerator, follow transitions/appends,
  disable-follow, nested scroll, image crop, nested wheel chaining,
  alternate-screen navigation indicator).
- The #1213 batch also compares the C++ alternate-screen scrollbar and search
  mouse routing against the audit-1211 `scrollbar.json` and
  `alt-screen-search.json` observations (referenced, not duplicated).
- The 1050 toolkit ledger rows for the 21 receipted members now name these
  scenarios; the receipt pins each row's production sources, comparison test
  sources, registered artifacts and the validation JUnit.
- The C++ comparisons live in the owning suites as `[issue1213]` cases that
  read the registered artifacts: `MouseEvidenceTest.cpp` (hit test, capture
  drag, wheel consumption, motion/click counts, regular screen),
  `MouseRoutingTest.cpp` (the SGR/X10/wheel parse spec, motion-then-wheel
  ordering, post-release capture clearing), `ScrollViewEvidenceTest.cpp`
  (wheel policy, follow-end indicator, no-follow-end gating) and
  `ScrollbarEvidenceTest.cpp` / `AltScreenSearchTest.cpp` (the alternate-screen
  routing against the audit-1211 observations).

## Validation

- Focused comparison selection: `ctest --preset vcpkg -LE architecture -L issue1213`,
  15/15 passed (`deliveries/1213-issue1213.xml`), run at
  `cace872ea8c25fbb49450766fa114e6e467174d1`.
- The ten receipts that pin the plan or the registry were revalidated under
  `revalidation1213` (see `1058-deliveries.json`).

## Capability status

The 21 receipted rows advance from historical-v1.0.4 or unverified records to
delivery-backed `verified` against the registered audit-1213 family and the
referenced audit-1211 observations. No membership or neighboring-row changes.

## Remaining boundary

Two rows stay unverified with a named blocker:
`TuiAltScreen.handleViewportInput` and
`TuiAltScreen.shouldDeferViewportInputToOverlay`. The frozen pi-v1.3.0
observation routes an unhandled wheel event to the transcript even while a
focused overlay owns viewport keys; the C++ `AltScreen` still returns early
through `should_defer_viewport_input_to_overlay()` and defers the wheel to
the focused overlay instead of scrolling the transcript. Both ledger rows
record `implementation: partial` with the gap, and the overlay-routing
observation (`audit-1050/overlay-routing.json`, scenario
`overlay-unhandled-wheel-scrolls-transcript`) pins the divergence. A whole-row
claim waits on the C++ implementation fix; per the batch boundaries this
comparison lane does not change behavior.

## 2026-11-06 disposition (#1229)

Retro-review blocker #1229 reconciled this receipt's evidence integrity. The
3 claims whose `discriminatingCase.test` string-equals one of the same
row's `comparisons[].test` entries now carry a non-empty `remainingBoundary`
entry in `1058-deliveries.json`: each claim stays validated but bounded and no
longer promotes its row, superseding the intermediate #1236 per-claim
withdrawal record for this receipt (removed; preserved in git history). The
rows revert to their source-audit status until a genuinely discriminating
negative/control/boundary case is delivered. No cited test, pinned input,
scenario mapping or routing digest changed; see
[1229-withdrawn-1209-root-tui-lifecycle-claims.json](1229-withdrawn-1209-root-tui-lifecycle-claims.json)
for receipts that also carry a #1229 withdrawal.
