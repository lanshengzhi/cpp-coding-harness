# #1220 B12 Layout algebra, scroll containers and small text components recaptured at pi-v1.3.0

## Scope

Route batch B12 of 17 (`docs/research/pi-v1.3.0/1182-tui-route-batches.json`,
disposition `comparison`). Recapture layout algebra, scroll containers, and the small text components
(Box, HStack, ScrollView, ScrollViewOptions, ScrollViewScrollToOptions, ScrollViewScrollbar, Spacer,
StackChild, StackEntry, StackEntryOptions, StackOptions, Text, TruncatedText, VStack, WheelScrollLines,
getOsc8LinkAtColumn, internal.layout, sliceByColumn, stripTerminalSequences, truncateToWidth,
utils:extractSegments, visibleWidth, wrapTextWithAnsi) at the frozen pi-v1.3.0 baseline
(`71023aec1546d6d677a65669c5665c0927ce0dd0`) and compare the C++ rows through the named callers
`cch::tui::detail::LayoutEngine`, `cch::tui::ScrollView`, `cch::tui::VStack`, `cch::tui::HStack`,
`cch::tui::Container`, `cch::tui::Box`, `cch::tui::Text`, `cch::tui::Spacer`, and `cch::tui::TruncatedText`
(`src/tui/Container.cpp`, `src/tui/HStack.cpp`, `src/tui/Layout.cpp`, `src/tui/ScrollView.cpp`,
`src/tui/StackLayout.cpp`, `src/tui/Text.cpp`, `src/tui/TruncatedText.cpp`, `src/tui/Utils.cpp`,
`src/tui/VStack.cpp`, `src/tui/AltScreen.cpp`). All 104 batch rows are receipted whole-row through the delivery contract.

## Delivered behavior

- The `audit-1220` family captures the v1.3.0 behavior across 4 registered artifacts (39 scenarios total):
  1. `layout.json` (13 scenarios):
     - `standalone-unbounded`: unbounded VStack allocation, gap spacing, and numeric basis clamping;
     - `frame-vertical-grow`: vertical grow weight distribution inside constrained frames;
     - `frame-vertical-shrink`: proportional shrink distribution and minimum size clamping;
     - `frame-nested-minimums`: auto-basis nested dock measurement against inner minimums;
     - `frame-cursor-crop`: leaf crop and cursor translation under vertical constraints;
     - `frame-responsive-visibility`: responsive visibility evaluation under changing frame viewports;
     - `frame-dock-growth`: transcript, editor, menu, and footer dock growth frames;
     - `hstack-unbounded`: standalone HStack alignment, growth, clamping, and zero-only entries;
     - `hstack-frame-unequal`: unequal column allocations across wide and narrow frames;
     - `hstack-frame-align`: cross-axis alignment (`stretch`, `start`, `center`, `end`);
     - `hstack-frame-auto-basis`: auto basis measured at full frame width;
     - `hstack-nested`: nested HStack frame geometry through mutations (growth, removal, re-addition);
     - `hstack-in-vstack`: HStack inside constrained VStack allocated from provided width.
  2. `scroll-view.json` (18 scenarios):
     - `wheel-accelerator`: velocity-based wheel acceleration and delta accumulation;
     - `follow-transitions`: follow-end transitions, unused scroll deltas, and state updates;
     - `follow-append`: content growth advancing a followed viewport window;
     - `scroll-to-disable-follow`: absolute scrollTo with disableFollow holding position at content end;
     - `nested-scroll`: nested viewport measurement, window containment, and depth ordering;
     - `scroll-image-crop`: scrolled image placement clipping and hidden source row offsets;
     - `nested-wheel-chaining`: wheel event consumption by inner viewport before chaining to outer;
     - `alt-screen-navigation-indicator`: primary viewport navigation indicators and release handling;
     - `alt-screen-implicit-follow`: implicit follow behavior on alternate screen;
     - `alt-screen-indicator-no-follow-end`: suppression of jump-to-end indicator on primary viewports without follow-end;
     - `alt-screen-auto-scroll-edge`: edge auto-scroll triggering and bounds clamping;
     - `alt-screen-auto-scroll-cancel`: cancellation of auto-scroll upon pointer release;
     - `alt-screen-auto-scroll-limit`: auto-scroll clamping at document limits;
     - `alt-screen-auto-scroll-limit-bottom`: auto-scroll clamping at bottom edge;
     - `alt-screen-auto-scroll-nested`: nested auto-scroll routing;
     - `alt-screen-auto-scroll-content-growth`: content growth handling during auto-scroll;
     - `scrollbar-modes-and-reservation`: `hidden`, `always`, and `auto` column reservation;
     - `scrollbar-transient-timeline`: transient scrollbar timeline, style hooks, and hover expansion.
  3. `component.json` (6 scenarios):
     - `styled-text-mutation`: Text render, setText, wrapTextWithAnsi, padding, and multiline ANSI wrapping;
     - `text-render-cache-and-bg`: Text render caching, customBgFn, tab normalization, and padding invalidation;
     - `box-rendering-and-cache`: Box render, paddingX/paddingY, applyBg, addChild, removeChild, clear, setPaddingX, setBgFn, and matchCache;
     - `box-mouse-routing`: Box handleMouse routing through padding to matching child components;
     - `spacer-rendering`: Spacer render empty lines, setLines, and invalidate;
     - `truncated-text-rendering`: TruncatedText single-line extraction, ellipsis truncation, and horizontal/vertical padding.
  4. `utils-width.json` (2 scenarios):
     - `visible-width-and-narrow-placement`: visibleWidth across ASCII, CJK, newlines, ANSI escapes, combining emoji; narrow rendering of Text and Input;
     - `ansi-boundary-order-and-hyperlinks`: wrapTextWithAnsi, truncateToWidth, sliceByColumn, getOsc8LinkAtColumn, and stripTerminalSequences.
  And `cch_tui:utils:extractSegments` is linked to `audit-1050/cursor-composition.json` (`truncated-line-drops-unterminated-fake-cursor`).
- The 1050 toolkit capability ledger rows for all 104 B12 members name these scenarios
  and their registered artifacts under `v1.3.0-observation`.
- C++ comparisons live in `tests/tui/StackLayoutEvidenceTest.cpp`, `tests/tui/HStackEvidenceTest.cpp`,
  `tests/tui/ScrollViewEvidenceTest.cpp`, `tests/tui/ContainerTest.cpp`, `tests/tui/UtilsTest.cpp`,
  and `tests/tui/CursorMarkerEvidenceTest.cpp` tagged `[issue1220]`. Every claim carries an independent
  discriminating case satisfying the #1229 contract.

## Validation

- Focused comparison selection: `ctest --preset vcpkg -LE architecture -L issue1220`,
  68/68 passed (`deliveries/1220-issue1220.xml`).
- The 22 receipts that pin `baselines.json` were revalidated under `revalidation1220`
  after the `audit-1220` registration.
- Full Validation: `cmake --build --preset vcpkg`, `ctest --preset vcpkg -LE architecture`,
  `ctest --preset vcpkg -L architecture`.
- `python3 -m unittest tests.evidence.ConsolidationAuditTest` passed;
  `python3 scripts/evidence/receipt_boundary.py` exit 0.

## Capability status

All 104 batch rows advance to delivery-backed `verified` against the registered
audit-1220 family.

## Remaining boundary

None. All 104 rows in route batch B12 are receipted whole-row.
