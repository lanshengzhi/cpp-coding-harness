# #1051 audit: Native TUI consumers and historical residuals

## Verdict

Frozen pi **v1.3.0** (`71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64/glibc) was captured through the real
`InteractiveMode` and native differential harness. The refreshed observations reproduce most historical residuals and
expose new v1.3.0 work: the settings selector now has 33 rows and changed wording, program status is absent from C++,
`outputPad` is only partially propagated, tool duration is not carried through results, and the native extension
lifecycle is only partially present.

This is an audit. It does **not** implement production fixes, modify or close any issue, or claim full alignment.
Missing C++ behavior is recorded as an implementation gap. Observations that require a PTY or a real terminal/emulator
(OSC 7501 bytes, dock-growth frames, image placement, full exit restoration) remain **unverified** here.

The parent correction of 2026-10-20 is applied: ADR 0075 and #1044 select native C++ extension **registration, hooks,
tool wrapping, run triggering and prompt/busy-state semantics**. ADR 0066's historical no-decision treatment is
superseded for those native behaviors. Only direct third-party TS/JS extension execution and the other explicit
ADR 0075 exclusions remain out of scope.

## Evidence and authority

- Baseline: `pi-v1.3.0` at `71023aec1546d6d677a65669c5665c0927ce0dd0`.
- Authority: ADR 0075 (`docs/adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md`),
  #1044, #1051, and the corrected #1049 ledger/report.
- Evidence family: `fixtures/pi-v1.3.0/audit-1051/` (manifest, three artifacts).
- Ledger: `docs/research/pi-v1.3.0/1051-native-tui-consumers-and-residuals.json` (46 rows).
- Private frozen checkout: `/tmp/cch-1051-pi-frozen` (clean at the revision above).
- Historical identity retained: `fixtures/pi-coding-agent/differential/report.json` (pi v1.0.4 at
  `7c10bd4337495ee613f2224843ecdf349b80d1df`) and `docs/research/tui-v1.0.4-closeout.md`. Neither file was edited;
  the v1.3.0 observations are a new baseline, not a relabel of the v1.0.4 bundle.
- The capture script refuses a modified checkout, a non-Linux/glibc host, a family registered to another baseline, or a
  checkout not at the registered revision.

### Captured artifacts

| Artifact | Scenarios | What it preserves |
| --- | ---: | --- |
| `fixtures/pi-v1.3.0/audit-1051/product-differential.json` | 17 | The existing native differential scenario list on the pi side: four boot widths, model/thinking/settings selectors, three editor cases, user message, tool result, footer, three resize cases and scrollback. Each report keeps visible cell text, scrollback text, cursor position/visibility and the resolved dark theme. |
| `fixtures/pi-v1.3.0/audit-1051/extension-editor.json` | 2 | A real inline extension installing a probe `EditorFactory` through `ctx.ui.setEditorComponent`: factory inputs, copied callbacks, text/focus transfers, appearance options, thinking/Bash border tokens, restore/reinstall, and the one-editor frame invariant. |
| `fixtures/pi-v1.3.0/audit-1051/program-status.json` | 1 | The exact `setProgramStatus` calls the interactive terminal receives for idle, blocked, working, done and error, including the dialog title and error first line. |

Manifest SHA-256: `e6f851edc266cf015506bea4dab68280b82be7fdc936bedb0980738a74a6646b`
(the manifest recorded the current capture at the time of writing; the registry patch in the handoff carries the final
value after the last capture).

### Reproduction

```bash
# Existing native differential harness (pi side, all 17 scenarios).
cd /tmp/cch-1044-worktrees/1051
PI_CHECKOUT=/tmp/cch-1051-pi-frozen PI_BASELINE=pi-v1.3.0 \
TSX_TSCONFIG_PATH=/tmp/cch-1051-pi-frozen/tsconfig.json \
/tmp/cch-1051-pi-frozen/node_modules/.bin/tsx \
  fixtures/pi-v1.3.0/audit-1051/capture-1051.mts

# Evidence bundle integrity (does not register the family).
python3 scripts/evidence/audit_evidence.py check \
  --manifest fixtures/pi-v1.3.0/audit-1051/manifest.json
```

The existing harness was also run directly for representative C++ cells (the `NativeTuiDifferentialTest` capture seam):

```bash
# One example; the same environment shape was used for boot-72/100, settings-selector,
# thinking-selector, user-message, tool-result and scrollback.
CCH_DIFFERENTIAL_SCENARIO=settings-selector \
CCH_DIFFERENTIAL_WORKSPACE=/tmp/1051-cpp-settings \
CCH_DIFFERENTIAL_WIDTH=100 \
CCH_DIFFERENTIAL_INPUTS='2f73657474696e6773|0d|53686f7720686172647761726520637572736f72|0d|15|5465726d696e616c2070726f6772657373|0d|15|1b' \
CCH_DIFFERENTIAL_OUTPUT=/tmp/1051-cpp-settings/out.json \
CCH_DIFFERENTIAL_AGENT_DIRECTORY=/tmp/1051-cpp-settings/xdg/config/pike/agent \
/home/lansy/cch-1051-build/cch_tests_coding_agent_interactive '[issue800]' -r compact
```

The C++ shell was configured from the pinned vcpkg root and built at `-j1` (host ceiling). No production source was
changed.

## Extension-editor observation (supersedes the source-only #1014 comparison for the pi side)

The pi capture installs a custom editor through the real extension UI context. Values observed:

- Factory inputs: a live TUI object (`requestRender`, `setFocus`), the editor theme with keys
  `borderColor`/`selectList`, and a `KeybindingsManager` with `matches`/`getEffectiveConfig`.
- Install: current text `before-install` is transferred; `onSubmit` and `onChange` are assigned; focus becomes `true`;
  `setPaddingX(0)`, `setAutocompleteMaxVisible(5)` and the autocomplete provider are applied once; 16 built-in action
  handlers are copied (clear, suspend, thinking/model/session actions, copy, follow-up, dequeue, external editor, ...).
- Typing: `typed-after-install` appends to the carried draft and the editor's `onChange` path runs.
- Thinking: shift+tab reaches `minimal`; the border sample changes from `\x1b[38;2;108;118;123mx\x1b[39m` to
  `\x1b[38;2;104;128;141mx\x1b[39m`.
- Bash mode: ctrl+c clears; `!` sets the Bash border `\x1b[38;2;94;178;134mx\x1b[39m`; deleting it restores the thinking
  border.
- Restore: `ctx.ui.setEditorComponent(undefined)` carries `after-install` back to the default editor, moves focus false,
  and leaves exactly two editor border rows (no second editor).
- Reinstall: carries `after-installdefault-typed`, restores focus true and reapplies the options; a second restore
  takes the now-cleared draft and leaves two border rows.
- Submit callback: from the reinstalled editor, the draft `after-installdefault-typedsubmitted-draft` invokes the
  copied `onSubmit` once, clears the editor and receives the deterministic assistant reply. The earlier restore
  transitions are therefore not synthesized by calling the callback directly.

C++ has the corresponding install/restore seam in `InteractiveView::set_custom_editor` and `ExtensionUiHost`; the
focused C++ `[extension_ui]` suite passes **17 cases / 321 assertions**. It covers text/focus/restore/options and
reload/session-replacement paths, but there is no Pike-side dual-runtime cell that records the factory inputs, copied
handlers or border-token values. That is why #1033 remains open; this artifact supplies the pi side, not the full
both-cells comparison.

## Program-status observation

With a recording terminal (the stock `VirtualTerminal.setProgramStatus` is a no-op), the interactive product emitted:

1. `{state:"idle", app:"pi"}`
2. `{state:"blocked", kind:"question", message:"Probe dialog", app:"pi"}` while an extension input dialog was open
3. `{state:"idle", app:"pi"}` after Escape
4. `{state:"working", app:"pi"}` for the first prompt
5. `{state:"done", app:"pi"}` after the response settled (no message because no session name was set)
6. `{state:"working", app:"pi"}` for the error prompt
7. `{state:"error", message:"deterministic failure", app:"pi"}` after the error response

The C++ tree has no OSC 7501 protocol, no `Terminal::set_program_status`, and no reporter. This is an explicit missing
capability, not an unverified presentation difference. The capture records the product's terminal-contract calls, not
raw OSC bytes; a real terminal/PTY is still required to accept the wire format.

## Workflow inventory and consumer status

The machine ledger is authoritative for every operation, owner, consumer, scenario and gap. Summary by workflow:

| Workflow | C++ consumer | Status | Main gap |
| --- | --- | --- | --- |
| Startup header/help | `InteractiveStartup`, `InteractiveView` | partial / captured | 72-column wrap (`! bash ·` vs `! bash`), missing static logo/docs rows |
| Loaded resources | `LoadedResources`, startup document | partial / captured | no Extensions section; `/reload` omits extensions |
| Project trust | `SessionFlowControllerTrust` | implemented / unverified | no fresh differential cell |
| Prompt/editor submit | `InteractiveView`, prompt slot | implemented / captured | no PTY cell |
| Autocomplete | `EditorAutocomplete` | implemented / unverified | no fresh differential cell |
| Input decoding | `InputDecoder`, `StdinBuffer` | implemented / captured | delayed-reply/shifted-key review remains |
| Prompt/busy lifecycle | `StatusIndicator`, event loop | partial / captured | extension busy/idle context missing (#1049) |
| Extension registration/tool source | `ExtensionToolRegistry`, `ExtensionUiHost` | partial / captured | no native hooks/run trigger/prompt-busy; no CLI loader producer |
| Extension editor | `ExtensionUiHost`, `InteractiveView` | implemented / captured | no both-cells comparison (#1033) |
| Extension prompt/status UI | `ExtensionUiHost` | partial / captured | dialogs/status facades absent; #1049 |
| Tool result | `ToolExecutionComponent`, renderers | partial / captured | mid-stream Working frame (#812), duration/outputPad |
| Tool duration | `ToolExecutionComponent`/`BashRenderer` | partial / captured | system clock, no `durationMs` through results/resume |
| Bash presentation | `BashRenderer` | implemented / unverified | outputPad, differential cell |
| Read line-range labels | `ReadRenderer` | partial / unverified | v1.3.0 normalization not tested |
| outputPad | `ChatContainer` | partial / unverified | only user/assistant/error, not tool/bash/custom |
| Model selector | `ModelSelector`, `ModelFlowController` | partial / captured | marker/layout; C++ post-Enter `Model:` assertion fails |
| Thinking selector | `ThinkingSelector` | partial / unverified | fixture map mismatch; common layout comparable, blank-row delta; availability unverified |
| Settings selector | `SettingsSelector`, `SettingsFlowController` | partial / captured | 13 vs 33 rows; stale hardware-cursor help; viewport |
| Theme tokens | `ThemeController`, `Theme` | partial / captured | v1.3.0 token/schema changes (#1049) |
| System theme | `ThemeController` | missing / unverified | no generator (#1032) |
| Hardware/fake cursor | `RenderPipeline`, `AltScreen`, `EditorLayout` | partial / unverified | fake cursor still drawn in hardware mode; stale help |
| OSC52 opt-in | `ClipboardWrite`, `ClipboardProcess` | partial / unverified | no `PI_CLIPBOARD_OSC52` |
| Clipboard paste | `ClipboardPaste`, `LinuxClipboard` | implemented / unverified | no Termux/PTY cell |
| Program-status protocol | none in `cch_tui` | missing / captured | no OSC 7501 |
| Program-status reporter | none in `cch_coding_agent` | missing / captured | no reporter/event wiring |
| Terminal progress | `Terminal::set_progress` | implemented / unverified | no byte-level PTY cell |
| Mode switch | `InteractiveEngine`, `TuiModeSwitchTest` | implemented / unverified | no fresh PTY switch |
| Transcript/scrollback | `ChatContainer`, `ChatViewport`, `AltScreen` | partial / captured | pi 12 vs C++ 9 retained rows |
| Selection reset | `AltScreen::clear_text_selection` | missing / unverified | no `resetTextSelection` equivalent or rebuild call |
| Exit/restoration | `Terminal` exit sequence | partial / unverified | no OSC 7501 clear; no real PTY |
| Images/Herdr | `AltScreen`, `TerminalImage` | partial / unverified | placement-span (#1035), no Herdr, no real emulator |
| Wheel under overlay | `MouseRouting`/`AltScreen` | missing / unverified | v1.3.0 wheel behavior absent |

### Fresh C++ observations (same scenarios)

The C++ capture seam was run for the scenarios that can settle a row. Normalized values, not rendered titles:

- `boot-72`: C++ first two rows are ` escape interrupt · ... ! bash ·` / ` ctrl+o more`; pi keeps `! bash` on the first
  row and starts the next with `· ctrl+o more`.
- `settings-selector`: C++ has 13 rows (`(1/13)`), pi has 33 (`(1/33)`). C++ includes `Show hardware cursor`, `Output
  padding` and `Terminal progress` but omits the v1.3.0 rows; its hardware-cursor description is the old
  `Show the terminal cursor while still positioning it for IME support`, while pi says `Use the terminal cursor instead
  of Pi's drawn cursor`.
- `thinking-selector`: the two captures are not an availability comparison. The C++ fixture uses
  `tests::make_full_thinking_model("faux-1")`, whose explicit `thinkingLevelMap` maps all seven levels, so
  `ai::get_supported_thinking_levels` returns `off/minimal/low/medium/high/xhigh/max`. The frozen pi fixture's
  `faux-1` is `reasoning: true` with no `thinkingLevelMap`; pi's `getSupportedThinkingLevels` therefore returns
  `off/minimal/low/medium/high`. The five common rows, title/help text, `→ ✓ off` marker and descriptions are
  directly comparable; a blank-row layout difference remains (pi has a blank row between the `>` prompt and first
  item, C++ does not). Availability parity is **unverified** until a matched-map scenario runs. The raw outputs are
  `/tmp/1051-cpp-thinking/out.json` (pike, seven levels) and `/tmp/1051-diff-thinking-selector/out.json` (pi, five
  levels); the committed pi artifact is unchanged.
- `scrollback`: after six scripted turns both have 10 non-empty visible rows, but pi retains 12 non-empty scrollback rows
  and C++ retains 9.
- `tool-result`: at the mid-stream barrier C++ already shows `deterministic tool answer`; pi still shows
  `── ⠋ Working ─...`. The final frame's read/result structure and `deterministic tool answer` match.
- `model-selector`: the C++ test fails `screen_contains("Model:")` after Enter (17 assertions passed, 1 failed), so no
  C++ output artifact was written for this scenario.
- Extension editor: C++ `[extension_ui]` passes 321 assertions/17 cases. The fresh pi values are listed above; there is no
  C++ cell that records the same factory/callback/border values.

## Historical residual reconciliation

All eight issues were read with their bodies and comments (`gh issue view ... --comments`); each was `OPEN` and none
had comments at read time. No issue was modified or closed. The exact identities below are the matching
scenario/capability names, not new tickets.

| Issue | Matching capability / scenario identity | Upstream v1.3.0 changed? | v1.3.0 observation | Recommendation |
| --- | --- | --- | --- | --- |
| #1043 | 72-column startup wrap (`boot-72`, `editor-long`, `editor-cjk`, `user-message`, `resize-*`); `scrollback`; `tool-result` timing; `settings-selector` viewport | **Mixed:** settings changed (33 rows, new descriptions); the wrap, scrollback and tool-timing families are unchanged | pi keeps `! bash` together and wraps `· ctrl+o more`; C++ leaves `! bash ·` and wraps `ctrl+o more`. Scrollback: pi 12 non-empty retained rows vs C++ 9. Settings: pi 33 vs C++ 13. | **Update #1043.** Split the settings viewport into its own actionable dependency; keep the other residuals. Do not preserve the old settings acceptance. |
| #1035 | Fullscreen image placement-span repaint | **No** placement-span change; the file has unrelated v1.3.0 selection/wheel/fake-cursor changes | No C++ placement-span capture; virtual-terminal tests only | **Reuse #1035.** Add the placement-span scenario and real Kitty/WezTerm observation; no source fix until the trigger is measured. |
| #1033 | Extension-editor lifecycle capture | **No** (`setCustomEditorComponent` unchanged) | Full pi-side values captured; C++ unit lifecycle passes; no both-cells cell | **Update #1033.** Attach `extension-editor.json`, add the Pike cell, compare factory/callback/focus/text/border values; do not close on the pi side alone. |
| #1032 | Generated `system` theme (`generateSystemThemeColors`, `terminalAppearance`) | **No** (`system-theme.ts` unchanged) | C++ still resolves only built-in dark/light | **Reuse #1032.** |
| #831 | Dock-growth footer frames (autocomplete/multi-line editor) | **No** reference change; this is a C++ `RenderPipeline` defect | No PTY A/B in this lane; focused C++ tests do not cover the reported frame sequence | **Reuse #831.** Add the VirtualTerminal frame test and the tmux/PTY observation. |
| #812 | Successful `tool-result` presentation | **Yes:** v1.3.0 adds tool duration and applies outputPad to transcript blocks; the final tool/result structure is still the reference | Final read/result structure matches; the mid-stream C++ frame paints the next prose row where pi still shows Working | **Update #812** with the duration/outputPad consequences and a stable-barrier case. |
| #810 | `thinking-selector` layout | **No** selector source change; v1.3.0 supports model-gated `xhigh`/`max` | C++ full-map fixture lists seven; pi no-map fixture lists five. Common five rows and `→ ✓ off` marker are directly comparable; a blank-row layout delta remains. Level availability is configuration-mismatched. | **Update/reuse #810.** Add a matched-map fixture for the layout/fixture comparison; do not add thinking levels from this capture. Record availability parity as unverified until both harnesses use the same map. |
| #808 | `model-selector` marker/row layout | **No** selector source change | pi shows `→ ✓ <model>`; C++ native differential fails its own post-Enter `Model:` assertion | **Reuse #808** and broaden it to the failing stable transition. |

## Reassessment of historical accepted differences and omissions

The new rule is: included behavior follows pi except the retained Magpie/XDG boundaries and the explicit ADR 0075
exclusions. Historical close-out allowances do not survive.

- **Settings viewport/omitted rows:** not accepted. The old `(1/13)` viewport, missing `Editor padding`,
  `Autocomplete max items`, `Auto-compact`, `Auto-resize images`, `Block images`, `Clear on shrink` and other v1.3.0 rows, and the
  stale `Show hardware cursor` description are defects. The 33-row v1.3.0 viewport is the target.
- **72-column startup wrap:** not accepted; still reproducible.
- **Scrollback retained-row delta:** not accepted; still reproducible (12 vs 9).
- **Tool-result mid-stream timing:** the final structure matches, but the mid-stream state is not accepted as a generic
  "capture-timing artifact" when the C++ frame has already advanced past pi. Keep #812/#1043 visible and compare at a
  stable barrier.
- **Startup logo/help and LoadedResources:** the static logo may be treated as the excluded pi branding animation, but
  the help-wrap and the native Extensions section are not branding. The extension section is now selected behavior and
  is missing from C++.
- **Hardware cursor:** the old `Show the terminal cursor while still positioning it for IME support` description is
  stale; v1.3.0 says `Use the terminal cursor instead of Pi's drawn cursor`. The fake-cursor behavior must match the
  selected v1.3.0 cursor contract.
- **outputPad:** applying it only to user/assistant/error is a partial implementation, not an accepted omission.
- **Native extensions:** the historical ADR 0066 "extensions outside the subset" treatment cannot exclude registration,
  hooks, tool wrapping, run triggering or prompt/busy-state behavior. The native tool source is implemented; the rest is
  a supported implementation gap.
- **Third-party TS/JS extensions and session share:** remain excluded. They must not be used to justify omissions in
  the selected native-extension or settings rows.

## Asynchronous projection, accounting and restoration

The consumer boundary is not just a static render tree. The rows that need an asynchronous/accounting check are:

- **Program status:** `ProgramStatusReporter` deduplicates event-derived status against dialog-derived blocked state;
  the observed order is idle -> blocked -> idle -> working -> done -> working -> error. The C++ consumer has neither the
  reporter state machine nor the terminal protocol, so no C++ ordering can be accepted yet.
- **Extension editor:** install/restore is synchronous through `setCustomEditorComponent`, but session replacement,
  deferred boot, `/reload`, in-flight submission and close are asynchronous lifecycle paths. The pi observation keeps
  text/focus/callback transitions across restore/reinstall; C++ `ExtensionUiHostTest` covers replacement/reload/close.
- **Tool duration:** v1.3.0 measures execution with a monotonic clock and carries `durationMs` through results, events,
  persistence/resume and rendering. C++ currently measures at render time with a system clock and only in the Bash
  renderer.
- **outputPad:** the setting is projected into every chat child and must re-render on change; C++ rebuilds only
  user/assistant/error components.
- **Transcript rebuild:** a selection/multi-click reset is a state transition before the next frame, not a rendering
  afterthought; C++ has `clear_text_selection` but no rebuild reset or multi-click-history reset.
- **Exit restoration:** the terminal must clear progress/program status, restore cursor/alternate screen/bracketed paste
  and emit the supported final transcript. C++ clears OSC 9;4 but has no OSC 7501 state to clear.

## Product versus toolkit responsibility

- **Toolkit (`cch_tui`/`frontend_tui`):** OSC 7501 protocol/detection, fake-cursor resolution, `resetTextSelection` and
  multi-click reset, wheel routing under overlays, Herdr detection/OSC8, image placement-span repaint, terminal
  restoration bytes, dynamic padding primitives.
- **Application (`cch_coding_agent`):** startup document and `/reload` content, settings selector rows/descriptions and
  persistence, model/thinking selector flows, transcript rebuild and selection-reset call, outputPad propagation,
  tool-duration carry through results/events/persistence/resume, OSC52 opt-in policy, program-status reporter/event
  ordering, extension UI facade and editor lifecycle, exit output policy.
- **Cross-owner:** program status needs the toolkit protocol + application reporter. Native extension registration needs
  the #1049 native lifecycle slices plus the TUI plugin-facing UI facade. Tool duration needs the Agent/tool boundary
  plus the TUI renderer.

## Explicit gaps and evidence limits

Missing C++ implementation is not treated as a completed comparison:

- No OSC 7501 program-status protocol or reporter (highest-severity new v1.3.0 gap).
- No `PI_CLIPBOARD_OSC52` opt-in.
- No generated system theme (#1032).
- No `durationMs` through results/events/persistence/resume; Bash uses a system clock.
- outputPad not applied to tool/bash/custom blocks.
- No transcript selection reset/rebuild call.
- No wheel-under-overlay behavior.
- No Herdr OSC8/image support.
- Native extension hooks/run triggering/prompt-busy-state missing (#1049); only the tool source and TUI
  editor/listener registration exist.
- Model selector native differential currently fails its own `Model:` assertion; no artifact was written for that run.
- PTY/real terminal/emulator observations are unavailable in this lane for OSC 7501 bytes, dock-growth footer frames,
  image placement/WezTerm, full exit restoration, and hardware-cursor behavior. They remain unverified rather than
  inferred from virtual-terminal output.

## Validation performed

- Capture script: **pass**, exit 0, 3 artifacts / 20 scenarios.
- `python3 scripts/evidence/audit_evidence.py check --manifest fixtures/pi-v1.3.0/audit-1051/manifest.json`:
  **pass**, prints the manifest SHA.
- C++ focused native differential cells: boot-100 (20 assertions), boot-72 (20), settings-selector (45),
  thinking-selector (27), user-message (20), tool-result (24), scrollback (30) — **pass**, except model-selector
  (17 passed, 1 failed: missing `Model:` after Enter).
- C++ `[extension_ui]`: **17 cases / 321 assertions pass**.
- #810 correction: exact captures re-read; C++ seven levels with an explicit full map, pi five with no map;
  availability parity remains unverified. No recapture or rebuild was needed.
- C++ `[cursor]`: **9 cases / 660 assertions pass**; `[exit]`: **20 cases / 2376 assertions pass**; `[footer]`:
  **16 cases / 152 assertions pass**.
- The C++ target was built at `-j1` from the pinned vcpkg root; no full suite was run for this audit-only change.

## Bounded recommendations and dependency candidates

1. Implement the toolkit OSC 7501 protocol/detection and the application reporter as one vertical dependency; verify
   raw bytes in a PTY.
2. Add monotonic tool `durationMs` at execute start and carry it through results, events, persistence, resume and
   rendering; keep model-response timing excluded.
3. Apply `outputPad` to every transcript block and add a 0-vs-1 distinguishing capture/test.
4. Update the settings selector to the v1.3.0 rows/descriptions/viewport; treat #1023's old 13-row viewport as
   superseded. Keep package/TS-extension and session-share rows out.
5. Add `resetTextSelection`/multi-click reset to the toolkit and call it on transcript rebuild.
6. Align hardware/fake cursor behavior and the settings help text; verify in a real terminal.
7. Add the `PI_CLIPBOARD_OSC52` opt-in branch.
8. Complete the #1049 native extension slices: registration/event dispatch, `before_agent_start` busy/idle context,
   prompt/custom-message contributions, run triggering and tool wrapping; do not add a TS/JS loader.
9. Port the generated system theme (#1032).
10. Add the placement-span image repaint and Herdr support only with real-emulator evidence (#1035).
11. Add wheel-under-overlay routing and a focused test.
12. Align model and thinking selectors using a matched thinking-level-map fixture; resolve the evidenced
    blank-row/layout difference, preserve the failing model post-Enter transition, and do not infer missing thinking
    levels from the unmatched captures.

These are dependency candidates for consolidation; this audit does not create or retarget their issues.
