# Interactive/rendering goldens

Committed C++-side screen goldens for the interactive/rendering surface
(issue #422, P26): the message pipeline and key flows pinned through the
VirtualTerminal seam with deterministic dimensions/environment, byte-compared
by `tests/coding_agent/tui/InteractiveRenderingGoldenTest.cpp`. The
interactive/rendering surface is C++-side only (pi keeps no golden renders —
G6 record, #394), so these screens are the committed byte-level gate.

- `message-pipeline.txt` — the full message/execution pipeline in one
  deterministic screen at 72×52: the compaction-summary message
  (`Compacted from 1,200 tokens`), a user message, an assistant message with
  thinking/text, the tool-execution block for a `read` call, the
  bash-execution block (`$ ls -la`), a custom `[notice]` message, and a
  `[branch]` branch-summary message.
- `model-switch.txt` — a key flow at 72×24: Ctrl+L opens the model selector,
  Down+Enter switches the session model to `beta-1`, with the `Model: beta-1`
  status and the footer's `(beta) beta-1` model.
- `fork.txt` — a key flow at 100×24: the in-session fork's user-message
  selector overlay (pi `showUserMessageSelector`) with the three fork
  candidates and `Message 3 of 3` preselected. 100 columns follow the
  selector-test convention (the F7 function-key dispatch needs the wider
  terminal — a pre-existing cch_tui input behavior below ~90 columns).
- `interrupt.txt` — a key flow at 72×24: `app.interrupt` aborts an active
  scripted run and the aborted assistant entry renders (`Operation aborted`).

The boot screen itself is pinned by the e2e goldens (`e2e/boot.txt`,
`e2e/turn.txt`, #399); the workspaces live at the deterministic
`cpp-harness-rendering-<name>` temp paths (recreated at boot) so the footer's
pwd line stays byte-stable.

These goldens were regenerated under the scrollback-flow renderer (ADR 0037,
#435) and reflowed by the app-layer full-buffer pass-through (#438): at #435
every composed buffer fit its viewport (72×52 pipeline, 72×24 / 100×24 key
flows), so the screens were byte-identical and overflow-into-scrollback was
not exercised at the composition seam. #438 removed the app-layer chat clip
(`InteractiveView::render` now passes the full conversation through), so the
five-message fork session's buffer at 100×24 now exceeds the viewport:
`fork.txt` re-captures the visible tail (the startup header scrolled into the
native scrollback and the selector overlay is viewport-positioned over the
conversation tail); the pipeline/model-switch/interrupt buffers still fit and
stay byte-identical. Scrollback behavior (full-buffer write, viewport-top
tracking, resize clear-screen + scrollback, image-follows-content, CRLF
native-scrollback flow) is pinned at the `ScreenStateGoldenTest` seam and the
`ProcessTerminalTest` scroll-flow test (#438).

**Known fork-B boundary (recorded, parity map #2).** On a real terminal,
images that begin in the terminal's scrollback (a resumed transcript with
history images, or scrollback-resident rows after a resize clear + reflow)
are not re-emitted: `ProcessTerminal::place_image` cannot address rows above
the current viewport top. Images placed while visible do follow their
content into the native scrollback in the grow case. The VirtualTerminal seam
pins the image-follows-content behavior (buffer-absolute placements); this
real-terminal boundary is the recorded fork-B Intentional Divergence and is
outside the text-only #438 acceptance.

The screens are plain cell text (styles are stripped by the VirtualTerminal).
Regenerate deterministically from the frozen checkout with the gate capture
sidecar (`fixtures/pi-coding-agent/capture/capture-gate-snapshots.mts`, which
drives `CCH_CAPTURE_GOLDENS=1 ./build/cch_tests_coding_agent_interactive "[issue422]"` and then
byte-verifies the result), or directly with the same command.
