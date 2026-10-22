# #1124 dated terminal evidence

Captured 2026-10-22T18:05:43.814Z on the current host through the real C++ `ProcessTerminal` implementation attached to a 100-column by 24-row pseudo-terminal. This records a PTY session launched from the host's Herdr terminal environment; it is not a capture made by a Herdr emulator process.

## Environment and detected capabilities

| Fact | Observation |
|---|---|
| Host `TERM` | `xterm-256color` |
| Host `TERM_PROGRAM` | `herdr` |
| Host `COLORTERM` | `truecolor` |
| Host `TERM_PROGRAM_VERSION` | `0.9.5` |
| PTY size | 100 columns × 24 rows |
| Detected hyperlinks | `true` |
| Detected inline image protocol | `1` (`Kitty`) |
| Detected color capability | `1` (`Xterm256`) |
| Detected synchronized output | `true` |

The actual PTY emitted these OSC 8 sequences for the markdown link and link closure, in hexadecimal:

```text
1b5d383b3b68747470733a2f2f6578616d706c652e636f6d2f6e6f7465731b5c
1b5d383b3b1b5c
1b5d383b3b07
```

With `PI_CLIPBOARD_OSC52=1`, the real clipboard writer emitted one OSC 52 sequence for `pty clipboard evidence`:

```text
1b5d35323b633b6348523549474e7361584269623246795a43426c646d6c6b5a57356a5a513d3d07
```

With `PI_CLIPBOARD_OSC52` unset, it emitted no OSC 52 sequence. Both runs used the real `write_clipboard_text` decision path and wrote to the PTY; helper search paths were empty so no desktop clipboard helper ran.

The timestamped raw byte record is retained outside the pi upstream evidence registry in `/tmp/cch-1044/handoffs/1124-terminal-capture.txt`. The executable case is `real PTY records Herdr markdown and OSC 52 terminal bytes` in `tests/coding_agent/tui/TerminalSessionCaptureEvidenceTest.cpp`.

## Limits

This is a real terminal session on this host, carried over a real PTY; the PTY itself does not emulate Herdr. The capture proves capability detection and emitted OSC bytes only. Nothing in the observation verifies that a terminal renders the hyperlink or performs the clipboard write. It is not a dated emulator screenshot, so the emulator-rendering portion of #1124 remains unverified.
