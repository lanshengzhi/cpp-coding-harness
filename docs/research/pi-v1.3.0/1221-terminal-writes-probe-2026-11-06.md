# #1221 terminal writes probe — 2026-11-06

This is a partial investigation, not a delivery receipt. No capability row is
promoted and no comparison obligation is replaced by this record.

The frozen checkout was at `71023aec1546d6d677a65669c5665c0927ce0dd0`, with
`node_modules/.bin/tsx` present. Its `packages/tui/src/terminal.ts` implements
`clearLine`, `clearFromCursor` and signed `moveBy` as direct stdout operations;
`moveBy(0)` emits nothing, positive values emit CSI B and negative values CSI A.
The C++ `Terminal` interface has no directly named equivalents for the first
two or for signed `moveBy`. `Terminal::move_cursor_down` only accepts an
unsigned downward distance and routes through `write`. A whole-row comparison
for those operations must identify the actual caller seam and exercise both
movement directions and zero; merely writing the expected bytes directly
would not test the operation. This is a seam to resolve, not proof that the
renderer lacks the corresponding behavior.

For `ProcessTerminal.clearScreen`, the already registered
`audit-1244-terminal-writes/terminal-writes.json:process-clear-screen` records
`ESC [ 2 J ESC [ H`, and the PTY test in `ProcessTerminalTest.cpp` compares
those exact bytes. The row is still unverified: the historical audit also
requires the generic `E02/E09/E12 terminal IO` bucket, which is not a captured
scenario. The issue's 2026-11-06 direction permits replacing that obligation
only by a named scenario and a dated, row-specific re-audit; it also requires
a separate genuinely discriminating test in the receipt. Neither replacement
nor a new receipt has been made here.

The comments on `Terminal::clear_screen` in
`src/tui/include/cch/tui/Terminal.hpp`, on `ProcessTerminal::clear_screen` in
`src/tui/ProcessTerminal.cpp`, and on scrollback in
`src/tui/include/cch/tui/VirtualTerminal.hpp` still describe the pre-#1244
combined screen/scrollback clear. They do not describe the separated byte
sequences documented in `1244-terminal-clear.md`; they should be corrected in
a lane that also revalidates affected pinned inputs.

Extending the existing capture changes its registered artifact and baseline
registry inputs. The previous #1244 capture extension required 24 receipt
revalidations, as recorded in `1244-terminal-clear.md`; this probe did not
measure the exact current revalidation set. The 1050 ledger is itself a pinned
receipt input, so an obligation replacement also needs the applicable receipt
revalidation and regenerated route index. The remaining terminal lifecycle and
rendered-effect rows still require the emulator evidence identified in issue
#1221; keybinding and remaining stdin rows retain the blockers in
`1221-terminal-io-delivery.md` and the issue comments. The byte-observable
rows are not blocked merely because those other rows need an emulator.
