# #1215 B7 Editor: layout, history, undo and kill/yank recaptured at pi-v1.3.0

## Scope

Route batch B7 of 17 (`docs/research/pi-v1.3.0/1182-tui-route-batches.json`,
disposition `comparison`). Recapture the historical pi-v1.0.4 editor
observations at the frozen pi-v1.3.0 baseline
(`71023aec1546d6d677a65669c5665c0927ce0dd0`) and compare the C++ rows through
the named caller `cch::tui::Editor` (src/tui/Editor.cpp) and
`detail::EditorLayout`. 80 of the 90 batch rows are receipted whole-row; the
remaining 10 stay unverified with named blockers below.

## Delivered behavior

- The `audit-1215` family captures the v1.3.0 behavior of the editor input
  machinery through the frozen checkout's own modules: the six
  editor-operations scenarios (coalesced unicode edit/undo, word navigation
  and jump, kill-ring accumulation and yank rotation, collapsed paste with
  atomic undo and submit, history draft recall, undo shortcut rebinding) in
  `screen-state.json`, and the completion-trigger observation (which inputs
  request suggestions, with which force flag and prefix text) in
  `editor-autocomplete.json`. The editor module is unchanged between the
  historical and frozen baselines; every observation is byte-identical to the
  pi-v1.0.4 bundle, and the recapture pins the named baseline revision.
- Rows whose behavior lives in neighboring families are referenced, not
  duplicated: the editor mouse scenarios (`editor-alternate-screen-click-...`,
  `editor-click-bounds-wrapped-segments-and-wide-graphemes`,
  `editor-drag-selects-and-copies-...`) through the registered
  audit-1212 `mouse-components.json`; the suggestion-list rendering, the
  visible-maximum cap and the padding/border frame through the registered
  audit-1201 `editor-component.json` scenarios; the whole render frame
  (constructor, theme, borders) through the registered audit-1050
  `cursor-composition.json` standalone scenario; and the search index through
  the registered audit-1211 `alt-screen-search.json` cache scenario.
- The 1050 toolkit ledger rows for the 80 delivered members now name these
  scenarios; the receipt pins each row's production sources, comparison test
  sources, registered artifacts and the validation JUnit.
- The C++ comparisons live in the owning suites as `[issue1215]` cases:
  `EditorTest.cpp` (the six editor-operations replays, the trigger replay,
  the audit-1201 cap/padding/list-control replays and the discriminating
  cases), `MouseComponentsTest.cpp` (the audit-1212 editor mouse cases plus
  the wrapped-click boundary case), `CursorMarkerEvidenceTest.cpp` (the
  standalone marker frame) and `AltScreenSearchTest.cpp` (the search index
  cache and wrap navigation).

## Validation

- Focused comparison selection: `ctest --preset vcpkg -LE architecture -L issue1215`,
  26/26 passed (`deliveries/1215-issue1215.xml`), run at
  `fcfcedf14` (the B7 implementation commit).
- The seventeen receipts that pin the registry were revalidated under
  `revalidation1215` after the audit-1215 registration (see
  `1058-deliveries.json`).
- Full Validation on the delivery tree: `cmake --build --preset vcpkg`, then
  `ctest --preset vcpkg -LE architecture` (4348/4348 passed) and
  `ctest --preset vcpkg -L architecture` (43/43 passed).
- `python3 -m unittest tests.evidence.ConsolidationAuditTest` 76/76 (including
  the #1229 stop-condition regression: no 1215 claim cites its happy-path
  test as the discriminating case); `scripts/evidence/receipt_boundary.py`
  exit 0 (25 receipts, 472 claims, 584 comparisons).

## Capability status

The 80 delivered batch rows advance from historical-v1.0.4 or
member-absent-from-diff records to delivery-backed `verified` against the
registered audit-1215 family and the referenced audit-1212/audit-1201/
audit-1050/audit-1211 scenarios. No membership or neighboring-row changes.

## Remaining boundary

Ten batch rows stay unverified with named blockers:

- `cch_tui:Editor.tui` — the TUI back-reference: the C++ Editor is constructed
  with options and sinks and holds no TUI member, and no frozen observation
  compares it (kept `not-audited`).
- `cch_tui:Editor.addNewLine`, `cch_tui:Editor.deleteToEndOfLine`,
  `cch_tui:Editor.deleteToStartOfLine`, `cch_tui:Editor.deleteWordForward`,
  `cch_tui:Editor.handleForwardDelete` — no frozen scenario presses a newline
  insertion, ctrl+k/ctrl+u, alt+d or the forward-delete key, so no v1.3.0
  observation of these bindings exists in this batch's families.
- `cch_tui:Editor.invalidate` — no frozen scenario observes invalidation (pi's
  own `invalidate()` is currently a no-op comment).
- `cch_tui:Editor.shouldSubmitOnBackslashEnter` — the frozen submit observation
  exercises the plain-Enter submit path; no backslash-enter sequence is pressed.
- `cch_tui:Editor.handleSlashCommandCompletion` and
  `cch_tui:Editor.isInSlashCommandContext` — provider-call granularity
  divergence: pi serializes same-tick request restarts through
  `autocompleteRequestTask`, so a stale restart short-circuits before the
  provider call (the frozen `/set` sequence observes exactly one
  `getSuggestions` call); the C++ completion session admits every restart
  into `start_request` eagerly and calls the provider once per keystroke
  (stale deliveries are still rejected by generation, so the settled state
  matches). The settled request of every slash-context case still matches the
  frozen entry in the trigger replay; only the call count differs. These rows
  await a completion-session coalescing fix.

The historical pi-v1.0.4 family stays registered under its own baseline; no
relabelling occurred.
