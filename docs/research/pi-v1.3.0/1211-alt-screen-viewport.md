# #1211 B3 alternate-screen viewport: frames, scroll algebra, scrollbar and search recaptured at pi-v1.3.0

## Scope

Route batch B3 of 17 (`docs/research/pi-v1.3.0/1182-tui-route-batches.json`,
disposition `comparison`). Recapture the historical pi-v1.0.4 alternate-screen
viewport observations at the frozen pi-v1.3.0 baseline
(`71023aec1546d6d677a65669c5665c0927ce0dd0`) and compare the C++ rows through
the named caller. All 57 batch rows are receipted whole-row.

## Delivered behavior

- The `audit-1211` family captures the v1.3.0 behavior of the alternate-screen
  viewport through the frozen checkout's own modules: the scrollbar modes,
  transient timeline, geometry, overlay splice and alternate-screen hover/drag
  (`scrollbar.json`), the fullscreen search box (`alt-screen-search.json`:
  normalized multi-row queries, ANSI/CJK columns, index cache, render
  controls, wrap navigation, close-restores-focus, box-drawing clicks,
  styled highlights, streaming resize) and the prompt/flash/indicator
  navigation (`alt-screen-nav.json`).
- Rows whose behavior lives in the shared scroll-view and selection machinery
  are compared against the audit-1213 `scroll-view.json` scenarios and the
  audit-1212 selection scenarios captured first by the #1213/#1212 batches
  (referenced, not duplicated); the cursor-marker row reuses the
  audit-1050-derived cursor composition scenario.
- The 1050 toolkit ledger rows for the 57 batch members now name these
  scenarios; the receipt pins each row's production sources, comparison test
  sources, registered artifacts and the validation JUnit.
- The C++ comparisons live in the owning suites as `[issue1211]` cases that
  read the registered artifacts: `ScrollViewEvidenceTest.cpp` (scroll
  algebra, follow transitions/appends, disable-follow, nested measurement
  and wheel chaining, image crop, implicit viewport, indicator gating),
  `ScrollbarEvidenceTest.cpp` (modes, timeline, geometry, overlay splice,
  hover, drag phases and layout-routing capture), `AltScreenSearchTest.cpp`
  (the search box families, including the v1.3.0 box-drawing comparison added
  with this lane), `AltScreenViewportTest.cpp` (prompt navigation, flash
  replacement, indicator gating), `SelectionAutoScrollTest.cpp` and
  `SelectionEvidenceTest.cpp` (the discriminating selection cases) and
  `CursorMarkerEvidenceTest.cpp` (cursor composition).

## Validation

- Focused comparison selection: `ctest --preset vcpkg -LE architecture -L issue1211`,
  34/34 passed (`deliveries/1213-issue1211.xml`), run at
  `cace872ea8c25fbb49450766fa114e6e467174d1`.
- The ten receipts that pin the plan or the registry were revalidated under
  `revalidation1213` (see `1058-deliveries.json`).

## Capability status

All 57 batch rows advance from historical-v1.0.4 or unverified records to
delivery-backed `verified` against the registered audit-1211 family and the
referenced audit-1213/audit-1212 scenarios. No membership or neighboring-row
changes.

## Remaining boundary

None for this batch. The historical pi-v1.0.4 family stays registered under
its own baseline; no relabelling occurred.

## 2026-11-06 disposition (#1229)

Retro-review blocker #1229 reconciled this receipt's evidence integrity. The
49 claims whose `discriminatingCase.test` string-equals one of the same
row's `comparisons[].test` entries now carry a non-empty `remainingBoundary`
entry in `1058-deliveries.json`: each claim stays validated but bounded and no
longer promotes its row, superseding the intermediate #1236 per-claim
withdrawal record for this receipt (removed; preserved in git history). The
rows revert to their source-audit status until a genuinely discriminating
negative/control/boundary case is delivered. No cited test, pinned input,
scenario mapping or routing digest changed; see
[1229-withdrawn-1209-root-tui-lifecycle-claims.json](1229-withdrawn-1209-root-tui-lifecycle-claims.json)
for receipts that also carry a #1229 withdrawal.
