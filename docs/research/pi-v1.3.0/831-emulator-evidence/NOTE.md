# #831 dock growth/shrink footer — dated real terminal-emulator A/B

Captured **2026-11-05** on this host (Linux x86-64, glibc 2.44, Wayland/Hyprland), both
runtimes' **real TUI** driven through identical dock growth/shrink steps inside **tmux** on a
real 50x30 PTY (tmux is a real terminal emulator; `capture-pane` records the exact rendered
grid, alternate screen included). This completes the issue's reopened acceptance criterion —
the dated real-PTY/emulator A/B growth frames the 2026-10-26 close-out explicitly left open
(it had only the pike-only PTY frames and virtual-terminal comparisons).

## Run and per-frame result

| Step | pike footer rows (28/29) | pi footer rows (28/29) |
| --- | --- | --- |
| 01 startup | `…/ws` + `0.0%/1.0M (auto) auto/glm-5.3-flash • medium` | `…/ws` + `0.0%/0 (auto) unknown` |
| 02 `/` menu | both rows intact | both rows intact |
| 03 nine-line paste (dock 9 rows) | both rows intact | both rows intact |
| 04 menu closed + C-u shrink | both rows intact (5 rows remain, identical in both) | both rows intact |
| 05 eighteen-line paste | both rows intact | both rows intact |
| 06 settled (paste collapses to `/[paste #1 +18 lines]` in both) | both rows intact | both rows intact |
| 07 resized 50x20 | both rows at the new physical bottom | both rows at the new physical bottom |
| 08 resized back 50x30 | both rows intact | both rows intact |

All 16 frames (`frames-pike/*.txt`, `frames-pi/*.txt`) keep the workspace path row directly
above the stats row at the physical bottom of the grid — including the intermediate frames
during growth and both resize transitions, which is exactly the "resize recovery cannot mask
an intermediate bad frame" clause: every frame is individually captured and individually good.

The two runtimes behave identically at each step; even the eighteen-line paste collapses to
the same single `/[paste #1 +18 lines]` notification row in both.

## Drive details

- **pike** — `build/pike` at `9603e2354` (provenance sidecar matches HEAD), `--no-session`,
  isolated `HOME`/`XDG_CONFIG_HOME`.
- **frozen pi** — real TUI from the frozen source checkout `../pi` at
  `71023aec1546d6d677a65669c5665c0927ce0dd0`, driven by the checkout's own tsx
  (`packages/coding-agent/src/cli.ts`). The checkout's committed `dist/` bundle is stale for
  this purpose (its theme resolver predates the okhsl themes and a generated provider data
  file is absent), so the observation uses the frozen *source* — the code the baseline
  actually names. The one gitignored build-generated file the source boot needs,
  `packages/ai/src/providers/data/azure.json`, was populated from the release
  `@earendil-works/pi-ai@1.3.0` package on this host; no tracked source was modified.
- Multi-line input was delivered as bracketed paste (`ESC[200~ … ESC[201~`), the same path
  the reporter's steps exercise.

## Boundaries (unchanged from the delivered close-out)

- The over-budget dock (~20 queued steering rows over a 30-line frame) does not reproduce via
  pasted editor text — both runtimes collapse a large paste to one notification row. That
  state remains measured on the delivered `VirtualTerminal` probe and the frozen
  `audit-831` capture; this A/B covers the fitting and growth frames.
- pi's stats row reads `unknown` because the isolated config root has no provider session;
  pike's reads the host gateway's default model. The footer-row *presence* is the compared
  property, not the model label.
- tmux's extended-keys warning is cosmetic for this drive (no modified-Enter keys were sent).

Driver: `drive-tmux.sh <pike|pi> <outDir>`.
