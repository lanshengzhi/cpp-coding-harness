# Regular renderer positioning without a DSR response

## Question

Can one regular-mode positioning strategy preserve shell content and update the same component rows when the initial physical cursor row is unknown and the terminal provides no DSR/CPR response?

## Method and observation

The audit ran a temporary Node probe against pi v1.0.4 (`7c10bd4337495ee613f2224843ecdf349b80d1df`), importing its `TuiMainScreen` and xterm-backed test `VirtualTerminal` directly. The terminal was 24 columns by 8 rows. Shell content was seeded with the physical cursor at row 1 and, independently, row 4. Neither case delivered a CPR response. `clearOnShrink` was disabled for these fixed-height update cases.

For each starting row, pi rendered `frame-one` and `frame-two`, then changed the first component row to `changed-one`. Assertions verified that the frame began at the original shell cursor row, the second component row stayed in place, and the shell content above the frame remained intact. Recorded toolkit output contained no `ESC[6n` DSR query or `ESC[3J` scrollback clearing sequence.

A minimal relative adapter used the same virtual terminal and initial states. It emitted the initial frame with CRLF, tracked only its own buffer-relative cursor row, and moved by the delta to the updated row before CR/line clearing. It produced the same expected shell and component rows in both cases, without observing the physical starting row.

| Starting physical row | First component row | Updated first row | Shell content above frame |
| --- | --- | --- | --- |
| 1 | `frame-one` at row 1 | `changed-one` at row 1 | Preserved |
| 4 | `frame-one` at row 4 | `changed-one` at row 4 | Preserved |

## Decision informed

The owner selected regular-mode relative line flow and fullscreen absolute viewport positioning in round 6 of the TUI alignment discussion. Regular mode removes the startup DSR dependency and does not retain a second positioning strategy as a permanent fallback. This is recorded in [accepted ADR 0067](../adr/0067-align-the-linux-tui-toolkit-with-pi-v1-0-4.md).

The observation supports the narrow unknown-origin question. It does not prove rendering equivalence under overflow, shrink, resize, image placement, overlays, IME cursor positioning, external cursor disturbance, or renderer switching. Those remain explicit differential acceptance scenarios for the implementation specification.

## Sources and temporary artifact

- [pi regular renderer](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui-main-screen.ts:134>).
- [pi xterm-backed virtual terminal](</home/lansy/Work/github/coding-agent/pi/packages/tui/test/virtual-terminal.ts:11>).
- [Current Pike DSR fallback](../../src/tui/ProcessTerminal.cpp:1228).
- [Existing absolute-flow decision](../adr/0041-own-the-anchored-absolute-flow-model-for-the-tui-main-screen-renderer.md).
- Executed command: `node /tmp/cch-tui-grill-vbqpr1/positioning.mjs`, using Node v26.11.1; all assertions passed. The temporary script is not a committed test or deliverable.
