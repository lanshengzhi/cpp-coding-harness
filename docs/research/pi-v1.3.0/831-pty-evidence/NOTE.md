# #831 dock-footer frames — dated real-PTY evidence

* **Captured (UTC):** 2026-10-23T03:19:24Z (frames 01-03 within the following minute)
* **PTY:** 50x30 pseudo-terminal via `.cursor/skills/verify-pike/helpers/pike_tui.py`
  (per-session broker owning the PTY master; raw ANSI stream in `raw.ansi`)
* **Binary:** `build/dev-fast/pike` at the reverted (pi-parity) `ticket/831-dock-footer` state
* **Isolation:** disposable `HOME`/`XDG_CONFIG_HOME` under a mktemp root (removed after capture)
* **Steps:** startup frame → `/` opens the slash-command menu (`02`) → bracketed 9-line
  paste grows the editor to 9 content rows + 2 borders (`03`)

Every frame keeps both footer rows: the workspace path row directly above the
`…%/1.0M (auto)  auto/glm-5.3-flash • medium` stats row, at the physical bottom of
the 30-row PTY. This matches the VirtualTerminal measurement and frozen pi
(`fixtures/pi-v1.3.0/audit-831/`): with a dock that fits the frame, no runtime
loses a footer row during autocomplete or multi-line growth.

## What this does NOT prove

* A PTY replay is not an emulator comparison: it proves the bytes pike writes to a
  kernel PTY at 50x30, not how a specific terminal emulator (xterm, kitty, …)
  scrolls or clips them. An emulator A/B remains unproduced.
* The over-budget dock (queued steering messages + menu + nine lines over a
  30-row frame) is covered only by the VirtualTerminal probe and the audit-831
  frozen-pi capture, not by this PTY drive.
* The model row reads `glm-5.3-flash` because this host has a live gateway; no
  model round trip was needed for the frames (nothing was submitted).
