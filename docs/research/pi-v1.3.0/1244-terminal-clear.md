# #1244 terminal clear and main-screen redraw

## Scope and delivered behavior

On 2026-11-06, against pi-v1.3.0 revision `71023aec1546d6d677a65669c5665c0927ce0dd0`, split the ProcessTerminal clear from the main-screen full redraw. Frozen `packages/tui/src/terminal.ts:567-569` clears the screen and homes (`ESC [ 2 J ESC [ H`); `packages/tui/src/tui-main-screen.ts:282-286` adds `ESC [ 3 J` within the synchronized redraw. The PTY test pins the first sequence exactly; the redraw test observes the second inside synchronized output. The virtual terminal preserves scrollback on direct clear and handles the renderer's separate clear bytes.

## Validation

The registered `audit-1210` selection passed 10/10 both before and after the change. This is not a byte-for-byte comparison of the resize redraw against the artifact; the focused redraw test instead pins its clear bytes through the render path. The affected #1182 selection passed 13/13. Their original receipts retain their earlier validation and carry `revalidation1244` entries with new JUnit artifacts and the specific changed input digest. Focused terminal, redraw, scrollback and backpressure recovery tests passed 4/4. Full `ctest --preset vcpkg` with `TERM_PROGRAM=herdr` and the lane lock selected 4557 tests: 4546 passed, 11 skipped, none failed. Consolidation build/check and `receipt_boundary.py` passed; the boundary check covered 31 receipts, 807 claims and 965 comparisons. Frozen evidence verification passed for 83 bundles, 179 artifacts and 1010 scenarios; format and diff checks passed.

## Capability status and remaining boundary

`cch_tui:ProcessTerminal.clearScreen` remains implemented/unverified. Its existing route is the historical pi-v1.0.4 family, with no registered pi-v1.3.0 capture or named scenario for this member. The source-cited exact-byte PTY assertion proves the behavior but does not meet the delivery contract's registered scenario-to-test mapping; a dedicated v1.3.0 capture and mapping are needed to promote the row. The ticket's required before-and-after full-redraw byte comparison against `audit-1210` remains outstanding. The existing #1221 delivery retains 16 implemented/verified rows; this change does not promote an additional row.
