# #831 — dock growth and shrink frames: measured finding, fix reverted

**Ticket:** `[pi-v1.3.0 B71 R831] dock 增减帧保留 footer`.
**Branch:** `ticket/831-dock-footer` (base `01e233281`).
**Answer in one line:** only fullscreen can lose a footer row; the row is missing from the
**composed buffer** (a dock-allocation and crop property, not a repaint bug); and frozen pi —
whose dock shape the C++ faithfully ports — loses the same row in the same frame. The ticket's
premise that both rows must survive every frame is stale; a `shrink: 0` "fix" was measured to be
a pi divergence and was reverted.

## What the ticket text gets wrong

The ticket names `viewport_height_changed`, `write_dock_lines` and `write_full_buffer`, and
blames the regular main-screen differential write. Those symbols have no hits in this tree: the
regular dock partition was deleted in #983 (`a0464bf50`), and `src/tui/RenderPipeline.cpp:23`
states "no dock partition". The only dock is the fullscreen one assembled by
`create_chat_viewport` (`src/coding_agent/tui/ChatViewport.cpp`).

## The probe

`tests/coding_agent/tui/DockFooterFrameTest.cpp` drives a real `InteractiveSessionRun` /
`InteractiveView` over a 50x30 `VirtualTerminal` and asserts **every** composed row after
**every** injection, in regular and in fullscreen mode:

1. `/` opens the autocomplete menu;
2. a bracketed nine-line paste grows the editor;
3. Enter submits so it shrinks;
4. a resize repaints the frame.

A second run gates the first streaming turn and queues twenty steering messages, the only way
the fullscreen dock's intrinsic height grows past the 30-row frame.

### Question 1 — which mode reproduces the loss?

**Fullscreen only.** Regular mode appends the footer to the same line flow as the transcript;
all regular-mode frames keep both rows. Fullscreen keeps a fixed dock whose rows are allocated
per frame, and there the footer can lose its second row.

### Question 2 — composition or repaint?

**Composition.** The row is absent from the composed buffer (the alt screen builds its frame per
physical row from the layout's boxes), so no later repaint restores it. A resize at the same
height does not repair it; only the dock shrinking again does — which is why the acceptance
asserts after every injection, not on the final screen.

The layout maths: every non-editor dock entry is `{shrink: 1, min_size: 0}`
(`ChatViewport.cpp:66` entry shape, `:144` the footer entry), so when the dock's intrinsic
height (20 pending + status + 11-row editor block + 2-row footer = 34) exceeds the 30-row
frame, `distribute` (`src/tui/StackLayout.cpp:41-83`) offers shrink to the two-row footer and
gives it at least one row per pass. The leaf crop (`src/tui/Layout.cpp:371-376`) keeps the
first rows, so the stats row leaves the frame and the path row is parked on the physical
bottom row.

## What frozen pi does (the decisive measurement)

`fixtures/pi-v1.3.0/audit-831/` captures frozen pi at `71023aec` driving the same frames at
50x30 through the real frozen toolkit:

| pi frame | footer rows |
| --- | --- |
| fitting dock: baseline, autocomplete open, nine-line paste, submit, resize | both, every frame |
| over-budget dock (20 pending + menu + nine lines): growth and post-resize | **stats row dropped**, path row parked on the physical bottom row |

pi's dock declares the footer `{ shrink: 1, minSize: 0 }`
(`packages/coding-agent/src/modes/interactive/chat-viewport.ts:31-44`) — exactly the shape the
C++ ports. **Pike before any fix reproduces pi's frame exactly** (stats absent, path row at the
bottom, dock content above). The ticket's requirement — both footer rows must survive every
dock growth and shrink frame — has no basis in pi; this is the stale premise, recorded as the
finding rather than fixed into a divergence.

## The reverted fix

A `shrink: 0` footer entry (never a shrink candidate) kept both rows on pike but diverged from
pi's measured behavior; a `min_size = 2` floor additionally padded one-row footers and broke
the frozen pi-v1.0.4 chat-viewport replay. Both were reverted; `ChatViewport.cpp` is byte-identical
to base. The deliverable is the measurement:

* `DockFooterFrameTest` pins it. `composed_rows` keeps every physical row (trailing spaces
  trimmed, empties kept), so a dropped row that leaves a blank hole fails instead of being
  skipped. `FooterExpectation::Both` asserts the last two content rows are contiguous
  `[path, stats]`; fullscreen additionally asserts the physical bottom two screen rows are
  exactly the footer. `FooterExpectation::StatsYields` pins the measured over-budget parity
  shape (stats absent, path row at the physical bottom) on the two over-budget frames, and the
  `released` frame returns to `Both` — a wrong frame repaired later still fails, and a future
  divergent "fix" fails on the StatsYields frames until pi changes.
* `docs/research/pi-v1.3.0/831-pty-evidence/` holds a dated real-PTY drive of the pike binary
  at 50x30 (startup, slash menu open, nine-line paste): both footer rows at the physical
  bottom in every frame.

## Tests

* `DockFooterFrameTest` — the per-frame acceptance in both modes (728 assertions): the four
  ticket steps plus the over-budget queued dock.
* Red for the strengthened assertion: with rows that skip empties (the reviewed form), a frame
  dropping a row and leaving a blank where it belongs passes; the kept-rows form fails it.
  Red for the parity pin: the reverted `shrink: 0` build fails the two StatsYields frames
  (stats present where pi yields it).
* The unit case "keeps both footer rows when the dock is over budget" was removed: it encoded
  the divergent fix.

## Evidence index

| artifact | what it proves |
| --- | --- |
| `fixtures/pi-v1.3.0/audit-831/` (unregistered; validator `check` emits the registry record) | frozen pi drops the stats row only when the dock is over budget; keeps both rows otherwise |
| `tests/coding_agent/tui/DockFooterFrameTest.cpp` | pike matches pi frame-for-frame at 50x30, per frame |
| `docs/research/pi-v1.3.0/831-pty-evidence/` | real-PTY frames at 50x30 keep both rows through the reporter's two steps |
| `831-dock-footer-evidence/red-over-budget-frame.txt` | the pre-fix failing frame's 30 composed rows |

## Honest limits

* The defect is a physical-row artifact. The VirtualTerminal and PTY captures prove the composed
  bytes; neither proves what a specific terminal emulator scrolls or clips. An emulator A/B
  remains unproduced.
* The over-budget state needs ~20 queued steering messages at 30 rows; the exact queue depth
  where a real session crosses the frame depends on terminal height and the pending block.
* With only the reporter's two steps (menu open, nine-line paste) at 50x30, no runtime loses a
  row — the loss needs the fuller dock. Which real session produced the reporter's frame is not
  reconstructable from the ticket.
