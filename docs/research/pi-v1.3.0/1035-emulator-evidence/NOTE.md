# #1035 placement-span repaint — dated real-emulator (Kitty) observation

Captured **2026-11-05** on this host (Linux x86-64, glibc 2.44, Wayland/Hyprland) in a real
**Kitty 0.49.2** window. This completes the issue's reopened acceptance criterion — the dated
Kitty/WezTerm verification that the 2026-10-26 close-out explicitly left open because it had
only virtual-terminal byte replays. WezTerm is not installed on this host, so the real-emulator
arm is Kitty with the WezTerm second-tier trigger selected by `WEZTERM_PANE` exactly as the
frozen capture (`fixtures/pi-v1.3.0/audit-1035/`) did; the control arm runs the same scenario
without `WEZTERM_PANE` in the same real Kitty.

## Scenario and drive

The frozen `audit-1035` scenario, unmodified: a 20x4 Kitty grid, one 100x100 PNG placement
(2 cols x 3 rows) covering rows [0, 3), then a text change on the covered row 1
(`changed`) and a control change outside every span (row 3, `done`). Both runtimes drive their
real fullscreen alt-screen implementation over the real PTY:

- **frozen pi** — `capture-placement-span-emulator.mts` drives a real `TuiAltScreen`; every
  write is mirrored to the real stdout. Capabilities detected from the real environment
  (`images: kitty`, `KITTY_WINDOW_ID=1`, `TERM=xterm-kitty`); the real grid is sized 20x4 via
  `stty` before painting. The frozen capture's own byte guards are re-asserted on the
  real-terminal stream.
- **C++ pike** — `capture_placement_span.cpp` (built against the Debug preset libraries; see
  below) drives a real `cch::tui::AltScreen` over a real `cch::tui::ProcessTerminal` in the
  Kitty window; it sets the 20x4 winsize itself (Kitty adopts the child-set winsize) and runs
  the same three phases.

Per-phase compositor screenshots were taken with `grim` (window geometry from `hyprctl`),
correlated to out-of-band phase markers; the pi byte stream per phase is recorded in
`pi-{wezterm,control}.json`/`.raw`. Driver: `run-capture.sh <wezterm|control> <outDir>`.

## Observation

Per-phase rendered frames (`{pi,cpp}-{wezterm,control}-{initial,covered-text-change,outside-span-change}.png`):

| Phase | pi bytes (image cmds) | C++ behavior | pi frame == C++ frame |
| --- | --- | --- | --- |
| initial | 702 (2) | upload + place, image rendered | yes (sha256) |
| covered-text-change, wezterm arm | 175 (2): `a=d,d=a` delete, erase pass, `changed`, `a=p` re-place | identical frame | yes (sha256) |
| outside-span-change, wezterm arm | 46 (0): no image commands | identical frame | yes (sha256) |
| covered-text-change, control arm | 49 (0): `ESC[2;1H ESC[2K changed` only | identical frame | yes (sha256) |
| outside-span-change, control arm | 46 (0) | identical frame | yes (sha256) |

Every matched phase frame is **SHA256-identical** between frozen pi and C++ pike (`b0db618c…`
initial, `aef84a8d…` covered, `1afdfb2c…` outside), in both arms: the real Kitty renders both
runtimes' byte streams to the same pixels.

**Positive result (wezterm arm).** A text change inside the live placement span repaints: the
erase pass, the `changed` text write and the `a=p` re-placement all execute in the real
emulator, and the rendered frame shows the new text over the intact placement.

**Negative result, with probes (control arm).** In real Kitty the repaint and no-repaint arms
render the *same* pixels — Kitty keeps the placed image visible under the written text, so the
missing second-tier re-placement has no visually distinguishable outcome. Per the acceptance
criterion, the negative result is recorded together with its probes rather than claimed as a
free "no difference": (1) the recorded real-terminal byte streams differ exactly as the frozen
contract requires — the wezterm arm carries the delete + erase + text + re-placement sequence,
the control arm carries a bare row rewrite with zero image commands; (2) the capture scripts
assert those byte-level guards on the real stream, so a run that silently stopped emitting the
repaint would fail the capture, not record it; (3) the teardown frame (`pi-control` shots)
shows the placement deleted on session stop, confirming the emulator really was compositing a
live placement that the byte stream governs. The cosmetic equivalence is a Kitty-specific
rendering property; the byte contract under it is the parity surface, and it holds in the real
emulator for both runtimes.

## Product finding fixed en route

The first C++ real-terminal run exposed a genuine product bug: `AltScreen::start()` read
`terminal_.capabilities().inline_images` **before** `terminal_.start()`, but a real
`ProcessTerminal` only detects its image protocol during `start()` — so a direct fullscreen
boot (`--tui-mode fullscreen`) showed the `[Image: …]` text fallback for the whole session,
while frozen pi detects capabilities at module load and places the image. Fixed in
`src/tui/AltScreen.cpp` (read the session protocol after the terminal start succeeds), with a
`LateCapabilityTerminal` regression case in `tests/tui/AltScreenPlacementSpanTest.cpp`
(`[issue1035]`): RED before the fix (fallback text, no `\x1b_G`), GREEN after (placement
emitted). This is the same class of evidence the virtual-terminal replay could never produce.

## Environment

- Kitty 0.49.2 (Wayland), `TERM=xterm-kitty`, `COLORTERM=truecolor`
- frozen pi checkout `../pi` at `71023aec1546d6d677a65669c5665c0927ce0dd0`, node v26.13.0, tsx from the checkout
- C++ harness built from this tree (`HEAD` at capture) against `build/libcch_tui.a` + `build/libcch_support.a`:

```bash
g++ -std=c++23 -g -fno-exceptions -I src/tui/include -I src/support/include -I src \
  docs/research/pi-v1.3.0/1035-emulator-evidence/capture_placement_span.cpp \
  build/libcch_tui.a build/libcch_support.a build/vcpkg_installed/x64-linux/debug/lib/*.a -lpthread
```

## Limits

- Kitty, not WezTerm: the criterion's "Kitty/WezTerm" is satisfied on the Kitty arm; WezTerm's
  own damage behavior (the regression shape the upstream test pins) remains unobserved on this
  host.
- The observation is one real Kitty version (0.49.2); other emulators/versions may composite
  the no-repaint arm differently — that is exactly why the byte contract stays the parity
  surface.
- The scenario is component-level (the frozen audit shape), not a full product session: no
  user-facing surface renders an arbitrary placement at 20x4.
- The C++ harness is an evidence generator, not a shipped target.
