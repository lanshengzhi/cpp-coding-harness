# #1209 B1 root TUI lifecycle, root mutation and diagnostic callbacks recaptured at pi-v1.3.0

## Scope

Route batch B1 of 17 (`docs/research/pi-v1.3.0/1182-tui-route-batches.json`,
disposition `comparison`). Recapture the historical pi-v1.0.4 root-TUI
lifecycle, overlay, cursor-marker and root mouse observations at the frozen
pi-v1.3.0 baseline (`71023aec1546d6d677a65669c5665c0927ce0dd0`) and compare
the C++ rows through the named caller. 113 of 113 batch rows are receipted
whole-row; none is left unverified.

## Delivered behavior

- The `audit-1209` family captures the v1.3.0 behavior of the root through
  the frozen checkout's own modules: the common-root add/remove/focus/input/
  clear/stop lifecycle and the styled frame with focused cursor
  (`screen-state.json`), the root extras (`root-extras.json`: key-release
  filtering, input listeners, debug and color-scheme write log, render
  controls, overlay bounds/focus transitions, the color-query late-reply
  delivery, capability predicates and overlay line composition), and the
  product cursor settings/mode-switch surface (`product-cursor.json`).
- The 1050 toolkit ledger rows for the 113 batch members now name the
  audit-1209 scenarios (or the reused audit-1210/audit-1213/audit-1201
  scenarios for the renderer-switch, mouse-grammar and palette rows); the
  receipt pins each row's production sources, comparison test sources,
  registered artifacts and the validation JUnit.
- The C++ comparisons live in the owning suites as `[issue1209]` cases that
  read the registered artifacts: `ScreenStateGoldenTest.cpp` (lifecycle and
  styled frame), `TuiTest.cpp` (root extras, including the late-reply
  delivery), `CursorMarkerEvidenceTest.cpp` (the fake-cursor marker contract:
  the frozen overlay/standalone/truncation scenarios separate resolved-cursor
  composition from marker leakage), `MouseEvidenceTest.cpp` and
  `MouseRoutingTest.cpp` (the frozen event grammar), `RendererSwitchEvidenceTest.cpp`
  (mode-switch frames) and `TerminalColorsTest.cpp` (palette assembly).
- Recorded divergence: the C++ late-reply sink fires once per late reply,
  while pi assembles the late replies into a single delivery once DA1 (or the
  full reply set) arrives. The comparable contract pinned by the comparison
  is the last delivery's assembled colors, which is what the frozen scenario
  observes.

## Validation

- Focused comparison selection: `ctest --preset vcpkg -LE architecture -L issue1209`,
  30/30 passed (`deliveries/1213-issue1209.xml`), run at
  `bf8e9c304bff398cb76b501016131bcd063b9e6f`.
- The ten receipts that pin the plan or the registry were revalidated under
  `revalidation1213` with their exact selections rerun into fresh JUnit
  artifacts (36/36, 19/19, 5/5, 9/9, 52/52, 16/16, 13/13, 89/89, 8/8, 3/3).

## Capability status

All 113 batch rows advance from historical-v1.0.4 or unverified records to
delivery-backed `verified` against the registered audit-1209 family and the
reused registered scenarios. No membership or neighboring-row changes.

## Remaining boundary

None for this batch. The historical pi-v1.0.4 family stays registered under
its own baseline; no relabelling occurred.
