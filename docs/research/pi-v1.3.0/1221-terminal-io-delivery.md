# #1221 B13: key identity and stdin framing at pi-v1.3.0

## Scope

Route batch B13 of 17 (`docs/research/pi-v1.3.0/1182-tui-route-batches.json`,
disposition `comparison`), two delivery slices. Recapture the key identity and
parse/match surface (slice A: nine rows) and the StdinBuffer framing surface
(slice B: seven promoted rows plus one bounded claim) at the frozen pi-v1.3.0
baseline (`71023aec1546d6d677a65669c5665c0927ce0dd0`) and compare them through the
public `cch::tui` seams. The two process-global Kitty state rows and the eleven
StdinBuffer member/type rows that are not independently receipted stay
unverified with the named blockers below.

## Delivered behavior

### Slice A — key identity, printable text and parse/match

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

### Slice B — StdinBuffer framing

- The `audit-1221-stdin-framing` family captures the v1.3.0 framing surface
  through the frozen checkout's own `StdinBuffer`:
  `stdin-framing.json` records seven scenarios — the options/event surface
  (default and explicit `timeout`/`escapeTimeout`, the `data`/`paste` event
  names and their order), paste start/end and collapsed paste across chunk
  boundaries, the buffer lifecycle (`getBuffer`, `clear`, `destroy`, flush of
  an empty fragment), `emitDataSequence` and
  `pendingKittyPrintableCodepoint` duplicate suppression, timeout versus
  escape-timeout selection with real timers, the `flush` return/timer-callback
  pair, and the registered fragmented-color reply recaptured at this baseline.
- The capture drives real Node timers: a lone ESC resolves at the 50 ms escape
  window while an incomplete CSI waits for the 300 ms sequence window; a
  recognized OSC 4 header (this family's only OSC capture; the OSC 7501
  program-status observation lives in `audit-1050`), XTVersion (`ESC P >|`) or
  Kitty graphics (`ESC _ G ...;`) header arms no timer and stays buffered until
  its terminator. This is the
  2026-11-06 continuation observation, not the stale 1050 gap: the C++
  implementation already keeps a complete recognized header buffered past the
  deadline. The three false 1050 gap strings that claimed a leaked partial
  OSC/APC/kitty tail were removed from the delivered rows.
- `tests/tui/StdinFramingEvidenceTest.cpp` reads the new family, the registered
  `audit-1221/input.json` fragmented-reply observations and the registered
  `audit-1050/keys-and-terminal-replies.json` fragmented-color observation. The
  real-deadline observation in this receipt is the
  `StdinBuffer::deadline()`/`flush()` route only; the independently scheduled
  ProcessTerminal PTY real-timer check is `Process Terminal resolves a lone
  escape within pi's escape fragment window` in
  `tests/tui/ProcessTerminalTest.cpp` (#722, outside this receipt).
- pi's `flush()` returns the held fragment without emitting; its timer callback
  emits it and reparses an ambiguous control-string prefix. The C++ seam
  combines both, so `StdinBuffer.flush` is compared as a combined
  flush-return-plus-delivery seam: the test checks the returned vector and the
  delivered data history against the two frozen observations, and the row is
  bounded rather than promoted because no C++ operation returns the fragment
  without delivering it.
- The capture's direct `emitDataSequence` call is replayed through `process`,
  the only public caller of that private method; `pasteMode`, `pasteBuffer` and
  `pendingKittyPrintableCodepoint` are compared through their paste, flush and
  duplicate-suppression effects because C++ has no accessor for the fields.

## Validation

- Implementation commit: `a4d33beaf82694ea1ed9df5b0d69c9ab3218f870`
  (capture, tests, 1050 rows, plan edits and the bounded-claim handoff). The
  receipt's `testedRevision` is this clean implementation commit; the delivery
  commit that follows carries only the regenerated evidence.
- Focused comparison selection:
  `ctest --preset vcpkg -LE architecture -L issue1221` selected 40 tests and
  passed 40/40 (`deliveries/1221-issue1221.xml`). `ctest --preset vcpkg -N -L
  issue1221` selects the same 40 cases (the slice A selection was 25).
- The #1201-terminal-primitives receipt pins the 1050 ledger changed by this
  review; its exact recorded selection was rerun under the lane identity into
  `deliveries/1201-terminal-primitives-revalidation-1221-stdin.xml` (9/9). The
  #1182 issue1182 selection was rerun after the route-batch index rebuild
  (13/13); the rebuild was byte-identical to the reviewed index, so the #1182
  receipt, its index pin and its prior revalidation records are unchanged. The
  22 other revalidations recorded by the original Slice B delivery remain in
  place because their pinned inputs did not move.
- Consolidation `build`/`check`: PASS, 1464 source rows, 609 delivery-backed
  rows (602 before slice A; slice B promotes seven and keeps
  `cch_tui:StdinBuffer.flush` bounded), 1007 scenarios (590 named by a row, 87
  covered by a referenced artifact, 330 explicit orphans, 0 uncovered).
- `python3 -m unittest tests.evidence.ConsolidationAuditTest`: 76/76 passed;
  `tests.evidence.TuiReconciliationTest`: 11/11 passed.
- `python3 scripts/evidence/receipt_boundary.py --baseline pi-v1.3.0 --receipt
  1221-terminal-io`: PASS (1 receipt, 17 claims, 60 comparisons). The full
  boundary check passes 31 receipts, 807 claims and 965 comparisons.
- `python3 scripts/evidence/audit_evidence.py verify --baseline pi-v1.3.0`:
  83 bundles, 179 artifacts and 1010 scenarios verified at the frozen
  revision.
- Full Validation on the delivery tree: `cmake --build --preset vcpkg -j4`
  passed. The full `CCH_LANE_NAME=lane-1221 TERM_PROGRAM=herdr ctest
  --preset vcpkg` run selected 4557 tests: 4546 passed, 11 skipped, 0 failed
  (exit 0). The skipped cases are the separately selected pi-interop,
  realtime-clock and Native TUI differential environments. An earlier full run
  hit one transient `TuiModeSwitchTest` cursor-restore failure
  (`pike_restored_ime` 4 vs 2); the case passed 1/1 in isolation and the final
  run was clean.
- `scripts/format-check.sh 6ffd1d053` and `git diff --check`: clean.

## Capability status

Sixteen B13 rows advance to delivery-backed `verified` against the
`audit-1221-key-surface`, `audit-1221`, `audit-1221-stdin-framing` and
`audit-1050` observations:

- Slice A: `cch_tui:Key`, `cch_tui:KeyEventType`, `cch_tui:KeyId`,
  `cch_tui:decodeKittyPrintable`, `cch_tui:isKeyRelease`,
  `cch_tui:isKeyRepeat`, `cch_tui:keys:shifted-symbol-matching`,
  `cch_tui:matchesKey` and `cch_tui:parseKey`.
- Slice B: `cch_tui:StdinBuffer`, `cch_tui:StdinBuffer.process`,
  `cch_tui:StdinBuffer.buffer`, `cch_tui:StdinBuffer.timeout`,
  `cch_tui:StdinBuffer.timeoutMs`, `cch_tui:StdinBuffer.escapeTimeoutMs` and
  `cch_tui:stdin-buffer:fragmented-terminal-replies`.
- `cch_tui:StdinBuffer.flush` is validated but bounded, not promoted: its three
  flush-calling comparisons and the empty-flush discriminating case pass, but
  the combined flush+callback seam below leaves it `upstream-captured` in the
  consolidated ledger.

No membership or neighboring-row changes.

## Remaining boundary

- `cch_tui:isKittyProtocolActive` and `cch_tui:setKittyProtocolActive` stay
  unverified: pi's functions are module-global
  (`../pi/packages/tui/src/keys.ts:25-40`), while the C++ implementation keeps
  protocol state per decoder (`src/tui/InputDecoder.hpp:68-69`) and `parse_key`
  takes the flag as an argument (`src/tui/include/cch/tui/Keys.hpp:74`). No
  global identity exists to compare.
- `cch_tui:StdinBuffer.flush` stays `upstream-captured`: pi's `flush()` returns
  the held fragment without emitting, and its timer callback emits and reparses
  it. `cch::tui::StdinBuffer::flush` combines both seams in one call
  (`src/tui/StdinBuffer.cpp:292-310`); the returned vector and the delivered
  data history are compared against the two frozen observations, but no C++
  operation can be observed returning the fragment without delivering it. The
  receipt's `remainingBoundary` names this combined flush+callback seam.
- `cch_tui:ProcessTerminal.stop` stays `upstream-captured` with the #1182
  finding recorded as a row-level gap: C++ gates the `ESC[>4;0m` modifyOtherKeys
  restore on `modify_other_keys_active`, so a Kitty-negotiated session omits the
  frozen stop sequence's final write (`1182-tui-reconciliation.md:101-119`;
  `src/tui/ProcessTerminal.cpp`). The remaining terminal families also have no
  v1.3.0 whole-row comparison. The route-batch index's B13
  `implementationObligation` is null because the generator counts every receipt
  claim, including this bounded claim, as delivered and therefore sees no
  implementation rows; the obligation now lives on the row.
- Eleven StdinBuffer rows stay unverified with named blockers:
  - `StdinBuffer.clear`, `StdinBuffer.destroy`, `StdinBuffer.getBuffer`: the
    class-level lifecycle scenario exercises them, but their member-specific
    whole-row scenario sets are not separately established in this slice; the
    class row is the delivery unit for the public lifecycle.
  - `StdinBuffer.constructor` and `StdinBufferOptions`: the behavior is compared
    through the `timeoutMs`/`escapeTimeoutMs` rows, but pi's option type
    (`timeout`/`escapeTimeout`) and the C++ struct (`timeout`/`escape_timeout`)
    are a representation difference, so the constructor and option type rows
    are not receipted.
  - `StdinBuffer.emitDataSequence` and
    `StdinBuffer.pendingKittyPrintableCodepoint`: private in both
    implementations; C++ has no accessor, so only the public `process` path's
    duplicate suppression is compared.
  - `StdinBuffer.pasteMode` and `StdinBuffer.pasteBuffer`: private fields; only
    their paste/flush behavior is compared.
  - `StdinBufferEventMap`: pi's typed EventEmitter event map has no C++ type
    identity; C++ exposes two handler slots.
  - `StdinBuffer.flush`: the combined flush+callback seam above.
- The other 81 B13 rows keep the slice blockers in the delivery plan: the 27
  keybinding rows need the mutable manager/global-identity and five-action
  assembly work, and the 54 terminal rows need the complete write/lifecycle
  capture, forwarded-input comparison and real terminal-emulator evidence.
