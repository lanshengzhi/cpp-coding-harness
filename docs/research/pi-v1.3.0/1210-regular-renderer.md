# #1210 B2 regular renderer, frame commit and render-state transfer recaptured at pi-v1.3.0

## Scope

Route batch B2 of 17 (`docs/research/pi-v1.3.0/1182-tui-route-batches.json`,
disposition `comparison`). Recapture the historical pi-v1.0.4 regular-renderer
observations at the frozen pi-v1.3.0 baseline
(`71023aec1546d6d677a65669c5665c0927ce0dd0`) and compare the C++ rows through
the named caller. 15 of 21 batch rows are receipted whole-row; 6 rows stay
unverified with the named blocker below.

## Delivered behavior

- The `audit-1210` family captures the v1.3.0 behavior of the regular
  renderer through the frozen checkout's own modules: the renderer-switch
  state and callback contract (`renderer-switch.json`), the product-cursor
  layout across mode switches (`product-cursor-layout.json`), and the public
  fullscreen-exit output (`fullscreen-exit-output.json`).
- The 1050 toolkit ledger rows for the 15 receipted members now name the
  audit-1210 scenarios (or the reused cursor-marker scenario for
  `TuiMainScreen.doRender`); the receipt pins each row's production sources,
  comparison test sources, registered artifacts and the validation JUnit.
- The C++ comparisons live in the owning suites as `[issue1210]` cases that
  read the registered artifacts: `RendererSwitchEvidenceTest.cpp`
  (state/callback contract, frame replay, live focus-preserving transfer),
  `TuiModeSwitchTest.cpp` (exact nested and clipped cursor coordinates across
  the switch), `FullscreenExitOutputTest.cpp` (the public exit-output
  families), `ProductCursorEvidenceTest.cpp` (settings resolution) and
  `CursorMarkerEvidenceTest.cpp` (the focused fake cursor under hardware
  cursor mode).

## Validation

- Focused comparison selection: `ctest --preset vcpkg -LE architecture -L issue1210`,
  10/10 passed (`deliveries/1213-issue1210.xml`), run at
  `cace872ea8c25fbb49450766fa114e6e467174d1`.
- The ten receipts that pin the plan or the registry were revalidated under
  `revalidation1213` (see `1058-deliveries.json`).

## Capability status

The 15 receipted rows advance from historical-v1.0.4 or unverified records to
delivery-backed `verified` against the registered audit-1210 family. No
membership or neighboring-row changes.

## Remaining boundary

Six Kitty-image bookkeeping rows stay unverified with a named blocker:
`TuiMainScreen.collectKittyImageIds`, `deleteChangedKittyImages`,
`deleteKittyImages`, `expandChangedRangeForKittyImages`,
`getKittyImageReservedRows` and `previousKittyImageIds`. The frozen
pi-v1.3.0 capture observes the regular renderer through the switch, layout
and exit-output families without disaggregating the Kitty image bookkeeping,
and the v1.0.4..v1.3.0 diff does not name these members
(`member-absent-from-diff`), so no honest frozen comparison exists for them
yet. They remain `unverified` awaiting either a dedicated capture or a
curated change-map entry.
