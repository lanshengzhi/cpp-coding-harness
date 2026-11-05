# #1216 B8 Input: editing, paste and submit recaptured at pi-v1.3.0

## Scope

Route batch B8 of 17 (`docs/research/pi-v1.3.0/1182-tui-route-batches.json`,
disposition `comparison`). Recapture the Input component editing, deletion,
kill ring, undo, escape, chunked paste, and rebound submit observations at the
frozen pi-v1.3.0 baseline (`71023aec1546d6d677a65669c5665c0927ce0dd0`) and
compare the C++ rows through the named caller `cch::tui::Input`
(src/tui/Input.cpp). All 30 batch rows are receipted whole-row through the
delivery contract.

## Delivered behavior

- The `audit-1216` family captures the v1.3.0 behavior of the single-line
  `Input` editing engine through the frozen checkout's own modules in
  `input-editing.json`:
  1. `input-grapheme-deletion-and-boundaries`: multibyte character and emoji
     deletion, left/right movement, and boundary backspace/delete no-ops;
  2. `input-line-kill-and-yank-rotation`: delete to line start (`ctrl+u`),
     delete to line end (`ctrl+k`), yank (`ctrl+y`), and yank-pop (`alt+y`)
     cycling through older kills;
  3. `input-word-navigation-and-deletion`: backward (`alt+b`) and forward
     (`alt+f`) word stepping across unicode and punctuation boundaries, and
     backward (`ctrl+w`) and forward (`alt+d`) word kills into the kill ring;
  4. `input-undo-coalescing-and-snapshots`: typing coalescing (word-character
     coalescing, whitespace boundaries) and sequential undo snapshots;
  5. `input-escape-cancel-and-invalidate`: escape and `ctrl+c` cancellation
     sink dispatch, and component invalidation;
  6. `input-paste-buffering-and-normalization`: chunked bracketed paste
     buffering, newline and carriage return stripping, tab expansion, and
     single-step atomic undo;
  7. `input-rebound-submit-and-fallthrough`: submit key rebinding, suppressing
     newline insertion on Enter while allowing raw LF to submit per pi's submit rule.
- Neighboring rows already observed by existing frozen families are referenced,
  not duplicated:
  - `standalone-input-fake-cursor-is-strippable` (audit-1050 cursor-composition);
  - `input-placeholder-reversal-and-custom-styling`, `input-raw-lf-submission-and-rebound-submit`,
    and `input-narrow-unicode-scroll-and-cursor` (audit-1050 input-standalone);
  - `input-press-cursor-and-unhandled-gestures` and
    `input-press-maps-grapheme-widths-and-scrolled-columns` (audit-1212 mouse-components).
- The 1050 toolkit capability ledger rows for all 30 B8 members now name these
  scenarios and their registered artifacts under `v1.3.0-observation`.
- C++ comparisons live in `InputTest.cpp`, `MouseComponentsTest.cpp`, and
  `CursorMarkerEvidenceTest.cpp` tagged `[issue1216]`. Every claim carries an
  independent discriminating case satisfying the #1229 contract.

## Validation

- Focused comparison selection: `ctest --preset vcpkg -LE architecture -L issue1216`,
  29/29 passed (`deliveries/1216-issue1216.xml`).
- The 18 receipts that pin `baselines.json` or `consolidation-plan.json` were
  revalidated under `revalidation1216` after the `audit-1216` registration.
- Full Validation: `cmake --build --preset vcpkg`, `ctest --preset vcpkg -LE architecture`
  (4352/4352 passed), `ctest --preset vcpkg -L architecture` (43/43 passed).
- `python3 -m unittest tests.evidence.ConsolidationAuditTest` 76/76 passed;
  `python3 scripts/evidence/receipt_boundary.py` exit 0 (26 receipts, 502 claims, 614 comparisons).

## Capability status

All 30 batch rows advance to delivery-backed `verified` against the registered
audit-1216 family and the referenced audit-1050 / audit-1212 scenarios.

## Remaining boundary

None. All 30 rows in route batch B8 are receipted whole-row.
