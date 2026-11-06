# #1221 B13 slice A: key identity, printable text and parse/match recaptured at pi-v1.3.0

## Scope

Route batch B13 of 17 (`docs/research/pi-v1.3.0/1182-tui-route-batches.json`,
disposition `comparison`), first delivery slice. Recapture the
key identity and parse/match surface at the frozen pi-v1.3.0 baseline
(`71023aec1546d6d677a65669c5665c0927ce0dd0`) and compare nine of the eleven
key rows through the public `cch::tui` seams. The two process-global Kitty
state rows stay unverified with the named blocker below.

## Delivered behavior

- The `audit-1221-key-surface` family captures the v1.3.0 key surface through
  the frozen checkout's own module:
  `key-constants.json` records every string entry of `Key` and every modifier
  helper applied to the base key `x`; `kitty-text.json` records
  `decodeKittyPrintable` for plain, shifted, keypad, astral, ctrl, alt, super,
  release, control-codepoint and non-CSI-u input; `release-repeat.json` records
  the raw `isKeyRelease`/`isKeyRepeat` suffix matrix, including the two
  suffix-shaped bracketed-paste strings and the `:3E`/`:2E` slots pi's matrix
  does not claim.
- Two native predicates, `cch::tui::is_key_release` and
  `cch::tui::is_key_repeat`, mirror `keys.ts:542-592`: the `\x1b[200~` paste
  exclusion first, then the exact suffix matrix. `parse_key` and `KeyEvent`
  are unchanged.
- `tests/tui/KeysSurfaceEvidenceTest.cpp` reads all five artifacts the slice
  needs: the three new-family artifacts, the registered
  `audit-1221/keys-kitty-text.json` identity observation and the registered
  `audit-1050/keys-and-terminal-replies.json` shifted-symbol observation. The
  existing `tests/tui/KeysTest.cpp` identity replay remains the comparison for
  `legacy-key-identity` and `kitty-active-key-identity`.
- The 1050 toolkit ledger rows for the nine delivered members name the ordered
  scenario sets; the consolidation plan no longer lists the two key-identity
  scenarios as explicit orphans.

## Validation

- Preflight: the existing shifted-symbol control passed before the receipt was
  built (`ctest --preset vcpkg -R "shifted-symbol reports consume the implicit shift"`,
  1/1).
- Focused comparison selection: `ctest --preset vcpkg -LE architecture -L issue1221`,
  25/25 passed (`deliveries/1221-issue1221.xml`), run at `ae3f9042` (the
  implementation commit). `ctest --preset vcpkg -N -L issue1221` selects the
  same 25 cases.
- The 23 receipts that pin the registry, the consolidation plan or the 1050
  ledger were revalidated under `revalidation1221Keys` after the registration;
  each exact recorded selection was rerun into
  `deliveries/<receipt>-revalidation-1221-keys.xml` and only drifted pinned
  input digests were refreshed. The #1182 receipt was rerun once more after
  the route-batch index was rebuilt from the final ledger, and its index pin
  and validation record were refreshed.
- Consolidation `build`/`check`: PASS, 1464 source rows, 602 delivery-backed
  rows (593 before this receipt), 1000 scenarios (580 named by a row, 87
  covered by a referenced artifact, 333 explicit orphans, 0 uncovered).
- `python3 -m unittest tests.evidence.ConsolidationAuditTest`: 76/76 passed;
  `tests.evidence.TuiReconciliationTest`: 11/11 passed.
- `python3 scripts/evidence/receipt_boundary.py --baseline pi-v1.3.0 --receipt 1221-terminal-io`:
  PASS (1 receipt, 9 claims, 20 comparisons). The full boundary check passes
  31 receipts, 799 claims and 925 comparisons.
- `python3 scripts/evidence/audit_evidence.py verify --baseline pi-v1.3.0`:
  82 bundles, 178 artifacts and 1003 scenarios verified at the frozen revision.
- Full Validation on the delivery tree: `cmake --build --preset vcpkg -j4`
  passed, then `ctest --preset vcpkg` selected 4539 tests: 4528 passed, 11
  skipped, 0 failed (exit 0). The skipped cases are the separately selected
  pi-interop, realtime-clock and Native TUI differential environments.
- `scripts/format-check.sh 218fc957a` and `git diff --check`: clean.

## Capability status

Nine B13 key rows advance to delivery-backed `verified` against the
`audit-1221-key-surface`, `audit-1221` and `audit-1050` observations:
`cch_tui:Key`, `cch_tui:KeyEventType`, `cch_tui:KeyId`,
`cch_tui:decodeKittyPrintable`, `cch_tui:isKeyRelease`,
`cch_tui:isKeyRepeat`, `cch_tui:keys:shifted-symbol-matching`,
`cch_tui:matchesKey` and `cch_tui:parseKey`. No membership or neighboring-row
changes.

## Remaining boundary

- `cch_tui:isKittyProtocolActive` and `cch_tui:setKittyProtocolActive` stay
  unverified: pi's functions are module-global
  (`../pi/packages/tui/src/keys.ts:25-40`), while the C++ implementation keeps
  protocol state per decoder (`src/tui/InputDecoder.hpp:68-69`) and `parse_key`
  takes the flag as an argument (`src/tui/include/cch/tui/Keys.hpp:74`). No
  global identity exists to compare.
- The other 99 B13 rows keep the slice blockers in the delivery plan: the
  StdinBuffer paste/lifecycle surface, the terminal write/lifecycle surface
  with real-emulator evidence, and the mutable keybinding manager gaps.
