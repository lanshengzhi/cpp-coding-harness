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
  final answer, and the footer's post-turn stats (P15).

The workspace lives at the deterministic `cpp-harness-e2e-workspace` temp
path (recreated at boot) so the footer's pwd line stays byte-stable.

Regenerate deterministically with `CCH_CAPTURE_GOLDENS=1 ./build/cpp_harness_tests "[issue399]"`.
The screens are plain cell text (styles are stripped by the VirtualTerminal);
the OSC 133 zones are asserted on the recorded output stream in the test, not
in these files. P24 wraps this directory into the `fixtures/pi-coding-agent/`
gate bundle (baseline pins, sanitization rules, capability checklist).
