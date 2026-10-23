# #831 — dock growth and shrink frames keep both footer rows

**Ticket:** `[pi-v1.3.0 B71 R831] dock 增减帧保留 footer`.
**Branch:** `ticket/831-dock-footer` (base `01e233281`).
**Answer in one line:** only fullscreen loses a footer row; the row is missing from the **composed
buffer** (a dock-allocation and crop problem, not a repaint problem), and only when the dock's intrinsic
height exceeds the frame.

## What the ticket text gets wrong

The ticket names `viewport_height_changed`, `write_dock_lines` and `write_full_buffer`, and blames the
regular main-screen differential write. Those symbols have no hits in this tree: the regular dock
partition was deleted in #983 (`a0464bf50`), and `src/tui/RenderPipeline.cpp:23` states "no dock
partition". The only dock that exists is the fullscreen one assembled by `create_chat_viewport`
(`src/coding_agent/tui/ChatViewport.cpp`).

## The probe

`tests/coding_agent/tui/DockFooterFrameTest.cpp` drives a real `InteractiveSessionRun` /
`InteractiveView` over a 50x30 `VirtualTerminal` and dumps **every** composed row after **every**
injection, in regular and in fullscreen mode:

1. `/` opens the autocomplete menu;
2. a bracketed nine-line paste grows the editor;
3. Enter submits so it shrinks;
4. a resize repaints the frame.

A second run gates the first streaming turn and queues twenty steering messages, which is the only way
the fullscreen dock's intrinsic height grows past the 30-row frame. `frames-summary.txt` records, per
frame, whether the footer's path and stats rows are present at all.

### Question 1 — which mode reproduces the loss?

**Fullscreen only.** Regular mode appends the footer to the same line flow as the transcript, so the
footer moves up with the content and never competes for space; all 26 regular-mode frames keep both
rows. Fullscreen keeps a fixed dock whose rows are allocated per frame, and there the footer can lose
its second row.

### Question 2 — composition or repaint?

**Composition.** The row is absent from the composed buffer (the alt screen's frame is built per
physical row from the layout's boxes), so no later repaint restores it. The frame that loses it looks
like this (`red-over-budget-frame.txt`):

```
R17|Steering: queued 17|
R18|── ⠙ Working ─────────────────────────────────────|
R19|/|
...
R27|line nine|
R28|/tmp/cpp-harness-catch2-…-…...|      <- path row survives
R29|                                     <- stats row never composed
```

The layout maths: every non-editor dock entry is `{shrink: 1, min_size: 0}`
(`ChatViewport.cpp:66` entry shape, `:144` the footer entry), so when the dock's intrinsic height (20 pending + status + 11-row
editor block + 2-row footer = 34) exceeds the 30-row frame, `distribute` (`src/tui/StackLayout.cpp:41-83`)
offers shrink to a two-row footer as well and gives it at least one row per pass. The leaf crop
(`src/tui/Layout.cpp:371-376`) keeps the first rows, so the stats row leaves the frame. A resize at the
same height does not repair it; only the dock shrinking again does — which is why a final-screen-only
assertion would pass on the broken build and why the test asserts after every injection.

## The fix

The footer is the one dock entry that never yields a row:

```cpp
// src/coding_agent/tui/ChatViewport.cpp
if (auto added = add_dock_region(*dock, std::move(options.footer), 0, 0); !added) {
```

`add_dock_region` gained a `shrink` parameter (default 1, the frozen entry). With `shrink = 0` the
footer is not a shrink candidate, so the pending, status and editor rows absorb the shrink (`+18 −7`
lines including comments). The footer's intrinsic height is untouched, so a shorter footer still
occupies only its own rows.

A `min_size = 2` floor was tried first and rejected: it pads a one-row footer to two rows, which
changes the frozen pi layout in the `viewport-scrollbar-*` evidence scenarios. `shrink = 0` expresses
the same guarantee with no padding and leaves the frozen replay green.

## Is this what pi does? No — it is a required fix

`fixtures/pi-v1.3.0/audit-831/` (unregistered, this family is not in `baselines.json`) captures frozen
pi at `71023aec` driving the same frames at 50x30:

| pi frame | footer rows present |
| --- | --- |
| fitting dock, all four ticket steps | both |
| over-budget dock, editor grown to 15 rows | **stats row dropped** |

Frozen `packages/coding-agent/src/modes/interactive/chat-viewport.ts:31-44` declares the footer
`{ shrink: 1, minSize: 0 }`, so pi loses the same row in the same frame and keeps the same first row
(the path row). Pike before the fix reproduced pi exactly. The ticket's requirement — both footer rows
survive every dock growth and shrink frame — is therefore an intentional divergence from pi, of the
kind recorded as a required fix; the parent can add the fix commit to
`fixtures/pi-coding-agent/capture/native-tui-differential.mts`'s `captureBase.includedFixes` if a
future capture relies on it.

## Tests

* `DockFooterFrameTest` — the per-frame acceptance in both modes: the four ticket steps (164
  assertions) and the over-budget queued dock (308 assertions).
* `ChatViewportTest` "keeps both footer rows when the dock is over budget" — the unit-level frame: a
  29-row dock over a 10-row viewport, where the shrink exceeds the pending and status capacity.
* Red evidence: `red-over-budget-frame.txt` (all 30 composed rows of the failing frame) plus the two
  unit checks. The final screen of the same run is correct, so the failure can only come from the
  per-frame assertion.

## Honest limits

* The defect is a physical-row artifact, and no `VirtualTerminal` can prove what a specific emulator
  does with a scroll region. What the harness proves is the composed frame — the row never reaches the
  buffer, so no emulator could paint it. What it cannot prove is the ticket's other reported symptom
  (a row pushed off the physical bottom and repaired by a resize); that needs a dated PTY A/B against
  a real emulator, recorded under `docs/research/pi-v1.3.0/` like the #808 captures.
* The over-budget frame is produced by twenty queued messages, which is a deliberate stress of the
  dock; the exact queue depth at which a real session crosses the frame depends on the terminal height.
