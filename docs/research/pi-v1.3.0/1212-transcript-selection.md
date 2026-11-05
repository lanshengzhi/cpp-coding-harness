# #1212 B4 alternate-screen transcript selection, multi-click and copy recaptured at pi-v1.3.0

## Scope

Route batch B4 of 17 (`docs/research/pi-v1.3.0/1182-tui-route-batches.json`,
disposition `comparison`). Recapture the historical pi-v1.0.4 alternate-screen
selection observations at the frozen pi-v1.3.0 baseline
(`71023aec1546d6d677a65669c5665c0927ce0dd0`) and compare the C++ rows through
the named caller. All 39 batch rows are receipted whole-row.

## Delivered behavior

- The `audit-1212` family captures the v1.3.0 behavior of transcript
  selection through the frozen checkout's own modules: the styled CJK
  wrapped selection, the content/focus lifecycle, the specific clipboard
  copy failure, copy-on-select defaults, the OSC 8 click/drag routing and
  the multi-click families (`selection.json`), and the concrete component
  press/wheel/submenu behaviors the selection machinery routes through
  (`mouse-components.json`).
- Rows whose discriminating behavior lives in the shared mouse routing and
  alt-screen reset machinery are compared through the audit-1213 `mouse.json`
  scenarios and the audit-1050-derived multi-click reset scenario
  (referenced, not duplicated).
- The 1050 toolkit ledger rows for the 39 batch members now name these
  scenarios; the receipt pins each row's production sources, comparison test
  sources, registered artifacts and the validation JUnit.
- The C++ comparisons live in the owning suites as `[issue1212]` cases that
  read the registered artifacts: `SelectionEvidenceTest.cpp` (styled CJK,
  lifecycle, copy failure, copy-on-select, multi-click routing),
  `SelectionAutoScrollTest.cpp` (edge ticks, re-armed selections, release
  and direction cancellation, content-growth validity), `AltScreenTest.cpp`
  (the selection reset that drops the multi-click history) and
  `MouseRoutingTest.cpp` (post-release capture clearing) and
  `MouseComponentsTest.cpp` (per-component click histories).

## Validation

- Focused comparison selection: `ctest --preset vcpkg -LE architecture -L issue1212`,
  28/28 passed (`deliveries/1213-issue1212.xml`), run at
  `cace872ea8c25fbb49450766fa114e6e467174d1`.
- The ten receipts that pin the plan or the registry were revalidated under
  `revalidation1213` (see `1058-deliveries.json`).

## Capability status

All 39 batch rows advance from historical-v1.0.4 or unverified records to
delivery-backed `verified` against the registered audit-1212 family and the
referenced audit-1213 scenarios. No membership or neighboring-row changes.

## Remaining boundary

None for this batch. The historical pi-v1.0.4 family stays registered under
its own baseline; no relabelling occurred.
