# #1219 B11 SelectList and SettingsList recaptured at pi-v1.3.0

## Scope

Route batch B11 of 17 (`docs/research/pi-v1.3.0/1182-tui-route-batches.json`,
disposition `comparison`). Recapture SelectList and SettingsList (including SelectItem,
SelectListLayoutOptions, SelectListTheme, SelectListTruncatePrimaryContext, SettingItem,
SettingsListTheme) at the frozen pi-v1.3.0 baseline
(`71023aec1546d6d677a65669c5665c0927ce0dd0`) and compare the C++ rows through the named callers
`cch::tui::SelectList` and `cch::tui::SettingsList` (`src/tui/SelectList.cpp`, `src/tui/SettingsList.cpp`,
`include/cch/tui/SelectList.hpp`, `include/cch/tui/SettingsList.hpp`). All 84 batch rows
are receipted whole-row through the delivery contract.

## Delivered behavior

- The `audit-1219` family captures the v1.3.0 behavior of SelectList, SettingsList, and their mouse
  interactions through the frozen checkout's own modules (`packages/tui/src/components/select-list.ts`,
  `packages/tui/src/components/settings-list.ts`, `packages/tui/src/tui.ts`):
  1. `select-list.json`:
     - `select-list-cycling-and-selection`: up/down navigation, wrap around top and bottom,
       enter selection dispatch, and selection change notification order;
     - `select-list-centered-five-row-window`: 5-row centered visible range calculation and scroll counters;
     - `select-list-description-width-boundaries`: multi-column layout, label and description spacing,
       and suppression of descriptions at or below 40 columns or 10 remaining description columns;
     - `select-list-narrow-wide-glyph-truncation`: whole-glyph wide character truncation;
     - `select-list-value-prefix-filter-and-selection-reset`: value prefix matching and selection reset to index 0;
     - `select-list-unicode-description-normalization`: single-line whitespace collapse and multi-line normalization;
     - `select-list-unbound-key-forwarding`: unmapped key pass-through without mutation;
     - `select-list-primary-column-layout-and-custom-truncation`: primary column width bounds, layout options,
       and custom truncatePrimary hooks receiving SelectListTruncatePrimaryContext.
  2. `settings-list.json`:
     - `settings-list-cycling-and-selection`: value control cycling on Enter and Space, wrapping, and escape cancellation;
     - `settings-list-centered-five-row-window`: 5-row centered windowing and scroll indicators;
     - `settings-list-description-and-hint`: multi-line description wrapping with ANSI, label padding capped at 36 columns,
       value column layout, and bottom hint line;
     - `settings-list-search-filtering-and-input`: single-line search input rendering, fuzzy filtering on label,
       selection reset, space typing vs activation, and empty search results;
     - `settings-list-submenu-navigation-and-lifecycle`: submenu opening on Enter, input and render delegation,
       and done callback handling with selectedValue and navigateTo;
     - `settings-list-update-value-and-select-item`: updateValue and selectItem mutations.
  3. `mouse-components.json`:
     - `select-list-press-selects-and-click-activates`: press selects row, click activates, and focus handling;
     - `select-list-wheel-hover-and-scrolled-rows`: wheel scrolling moves one row per notch, and hover never mutates selection;
     - `settings-list-press-selects-and-click-activates`: press selects setting row, click activates;
     - `settings-list-wheel-hover-and-scrolled-rows`: wheel scrolling clamps at bounds, hover is inert;
     - `settings-list-search-row-forwards-and-separator-is-inert`: search row clicks forward to Input and separator is inert;
     - `settings-submenu-click-closes-and-keeps-host-focus`: submenu mouse events forward to submenu component and restore focus.
- The 1050 toolkit capability ledger rows for all 84 B11 members name these scenarios
  and their registered artifacts under `v1.3.0-observation`.
- C++ comparisons live in `tests/tui/SelectListTest.cpp`, `tests/tui/SettingsListTest.cpp`, and
  `tests/tui/MouseComponentsTest.cpp` tagged `[issue1219]`. Every claim carries an independent
  discriminating case satisfying the #1229 contract.

## Validation

- Focused comparison selection: `ctest --preset vcpkg -LE architecture -L issue1219`,
  32/32 passed (`deliveries/1219-issue1219.xml`).
- The 21 receipts that pin `baselines.json` were revalidated under `revalidation1219`
  after the `audit-1219` registration.
- Full Validation: `cmake --build --preset vcpkg`, `ctest --preset vcpkg -LE architecture`,
  `ctest --preset vcpkg -L architecture`.
- `python3 -m unittest tests.evidence.ConsolidationAuditTest` passed;
  `python3 scripts/evidence/receipt_boundary.py` exit 0.

## Capability status

All 84 batch rows advance to delivery-backed `verified` against the registered
audit-1219 family.

## Remaining boundary

None. All 84 rows in route batch B11 are receipted whole-row.
