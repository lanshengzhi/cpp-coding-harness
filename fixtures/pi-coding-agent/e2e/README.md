# Interactive-boot-through-an-agent-turn E2E goldens

Committed CLI-level screen goldens for the interactive E2E (issue #399):
the VirtualTerminal-driven boot and the post-turn screen after a fake
`ModelRuntime` scripted turn (thinking + edit tool call + diff result +
final answer), byte-compared by
`tests/coding_agent/tui/InteractiveBootE2ETest.cpp`.

- `boot.txt` — the boot screen at 72x24: pi's main-screen composition
  (header keybinding hints only, no logo; chat with the initial Agent
  Session snapshot; the two-row idle status; the bordered editor; the
  two-line footer with the workspace and stats) before any input.
- `turn.txt` — the same terminal after a focused-editor submission streams
  the scripted turn: the user message in the pi box shape, the assistant's
  thinking/text, the edit tool-execution block with the diff renderer, the
  final answer, and the footer's post-turn stats (P15). At 72x25 the composed
  buffer now overflows the viewport once the turn streams (the full
  conversation passes through under ADR 0037), so the startup header has
  scrolled into the terminal's native scrollback and this golden records the
  visible tail.

The workspace lives at the deterministic `cpp-harness-e2e-workspace` temp
path (recreated at boot) so the footer's pwd line stays byte-stable.

These goldens were regenerated under the scrollback-flow renderer (ADR 0037,
#435) and reflowed by the app-layer full-buffer pass-through (#438): at #435
`boot.txt` and `turn.txt` were byte-identical to the pre-regeneration screens
because each composed buffer fit its viewport, so overflow-into-scrollback
was not exercised at the composition seam. #438 removed the app-layer chat
clip (`InteractiveView::render` now passes the full conversation through), so
the post-turn buffer at 72x25 exceeds the viewport: `turn.txt` re-captures
the visible tail (header scrolled away) and `boot.txt` stays byte-identical
(no conversation yet, buffer fits). Scrollback behavior (full-buffer write,
viewport-top tracking, resize clear-screen + scrollback, image-follows-
content, CRLF native-scrollback flow) is pinned at the `ScreenStateGoldenTest`
seam and the `ProcessTerminalTest` scroll-flow test (#438).

Regenerate deterministically from the frozen checkout with the gate capture
sidecar (`fixtures/pi-coding-agent/capture/capture-gate-snapshots.mts`, which
drives `CCH_CAPTURE_GOLDENS=1 ./build/cch_tests_coding_agent_interactive "[issue399]"` and then
byte-verifies the result), or directly with the same command. The screens are
plain cell text (styles are stripped by the VirtualTerminal); the OSC 133
zones are asserted on the recorded output stream in the test, not in these
files. P24 wrapped this directory into the `fixtures/pi-coding-agent/` gate
bundle (baseline pins, sanitization rules, capability checklist — see the
bundle README).
