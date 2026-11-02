# #1182 Linux TUI comparison reconciliation

## Scope

Reconcile the 920 supported `cch_tui` capability rows with their current delivery evidence and
register complete whole-row comparison receipts where the full row is covered. Authority:
[#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044),
[#1183](https://github.com/lanshengzhi/cpp-coding-harness/issues/1183),
[ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md),
Named Baseline **pi-v1.3.0** at `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc.

Bounded verification scope: the reusable Linux TUI Owner (`cch_tui`), its already-delivered
comparison evidence and its selected product consumers. This packet implements no neighboring
feature, changes no capability membership, and does not promote a row whose recorded obligation is
not compared. Membership, implementation and verification stay separate.

The 920-row count is a reconciliation inventory, not a completion percentage and not a demand to
redesign every operation. Of those rows, 869 were `unverified` and 51 `upstream-captured` before
this pass; 828 carry the historical pi-v1.0.4 comparison route whose recorded scenario label is a
coarse capability family (`E07 SelectList`, `E13-E15 fullscreen viewport`, …) rather than a
registered scenario of the pi-v1.3.0 baseline. Those labels cannot anchor a whole-row receipt by
construction, so the remaining work is a capture-and-compare obligation, and this packet names it
batch by batch instead of leaving it to
[#1126](https://github.com/lanshengzhi/cpp-coding-harness/issues/1126).

## Reconciliation method

1. Read every supported `cch_tui` row in
   [1058-consolidated-capability-ledger.json](1058-consolidated-capability-ledger.json) and resolve
   each recorded comparison scenario against the registered pi-v1.3.0 artifact registry
   (`fixtures/pi-v1.3.0/baselines.json`, 55 families, 115 artifacts, 649 scenarios). The source
   audit facts in [1050-toolkit-capability-ledger.json](1050-toolkit-capability-ledger.json) and
   [1051-native-tui-consumers-and-residuals.json](1051-native-tui-consumers-and-residuals.json)
   were preserved as history and were not treated as current code observations.
2. A row can carry a whole-row receipt only when **every** recorded scenario resolves to a
   registered artifact, a passing CTest case compares that observation's fields, and a
   discriminating case separates a shallow implementation. 54 rows resolve; only 19 of them are also
   `implemented` (the receipt contract rejects a verified row that is not implemented).
3. Each of those 19 was executed. Twelve did not complete: eleven are blocked by the recorded stop
   sequence divergence below and one (`prompt-input-decoder`) is only observed by the external
   dual-runtime harness, which skips without its capture environment. The remaining **7** are
   registered as one pinned delivery record.
4. Every other row is assigned to exactly one bounded comparison batch in
   [1182-tui-route-batches.json](1182-tui-route-batches.json). The assignment is generated from the
   ledger by `fixtures/pi-v1.3.0/consolidation-1058/tui_route_batches.py` and validated by
   `tests/evidence/TuiReconciliationTest.py`, so a row cannot fall out of the index and a batch
   cannot lose its owner, public caller, scenario, normal/discriminating case, dependency or
   implementation obligation without failing a test.

## Delivered behavior

### Complete delivered rows (7)

One delivery record, `1182-tui-comparisons`, in
[1058-deliveries.json](1058-deliveries.json), pins the production and test sources, the registered
frozen artifacts and manifests, the evidence readers, the CTest JUnit result and the resolved
scenario routing. Each row's `remainingBoundary` is empty.

| Row | Frozen artifact | Scenario | C++ case(s) |
| --- | --- | --- | --- |
| `cch_tui:TUI.getShowHardwareCursor` | `audit-1050/hardware-cursor.json` | `main-screen-default-hidden` | `frozen hardware-cursor observations replay across MainScreen and AltScreen` (`tests/tui/HardwareCursorEvidenceTest.cpp`) |
| `cch_tui:TUI.setShowHardwareCursor` | `audit-1050/hardware-cursor.json` | `main-screen-toggle` | `frozen hardware-cursor observations replay across MainScreen and AltScreen` (`tests/tui/HardwareCursorEvidenceTest.cpp`) |
| `cch_tui:ProgramStatus` | `audit-1050/program-status-format.json` | `format-program-status-encodes-fields-and-limits` | `Program status format encodes frozen OSC 7501 fields and limits` (`tests/tui/ProgramStatusTest.cpp`) |
| `cch_tui:formatProgramStatus` | `audit-1050/program-status-format.json` | `format-program-status-encodes-fields-and-limits` | `Program status format encodes frozen OSC 7501 fields and limits` (`tests/tui/ProgramStatusTest.cpp`) |
| `cch_tui:Text` | `audit-1050/padding-and-selection-reset.json` | `text-set-padding-x-rerenders-dynamic-width` | `Text re-wraps at a new horizontal padding like frozen pi` (`tests/tui/ContainerTest.cpp`) |
| `cch_tui:Text.setPaddingX` | `audit-1050/padding-and-selection-reset.json` | `text-set-padding-x-rerenders-dynamic-width` | `Text re-wraps at a new horizontal padding like frozen pi` (`tests/tui/ContainerTest.cpp`) |
| `cch_tui:TuiAltScreen.applySelection` | `audit-1050/selection-multiclick.json` | `styled-cjk-wrapped-selection` | `AltScreen selection replays styled CJK cells across wrapped rows and copies frozen text` (`tests/tui/SelectionEvidenceTest.cpp`) |

The discriminating cases are `ITui roots support hardware-cursor preference and IME positioning`
(`tests/tui/ITuiTest.cpp`), `Program status reply recognition accepts only the frozen query echoes`
(`tests/tui/ProgramStatusTest.cpp`), `Box padding change invalidates the render cache`
(`tests/tui/ContainerTest.cpp`) and `AltScreen selection preserves the cell range through content
and focus changes` (`tests/tui/SelectionEvidenceTest.cpp`). Each receipt claim carries the reason
it separates the delivered behavior from a plausible shallow one: a preference that is always
hidden, a parser that accepts a report as a query, a composition that re-renders without
invalidating, and a highlight pass that forgets the cell range.

### Strengthened comparisons

Four cases claimed a frozen observation they did not read, so their comparison could only fail
through a receipt digest, never through the assertion. They now read the registered artifact:

| Case | Change | Frozen fields now compared |
| --- | --- | --- |
| `Program status format encodes frozen OSC 7501 fields and limits` | reads `audit-1050/program-status-format.json` | `blocked`, `clear`, `omitted`, `controlCharacters`, `controlCharactersMessage`, `longMessageBytes`, `longMessageDecodedLength` |
| `Text re-wraps at a new horizontal padding like frozen pi` | reads `audit-1050/padding-and-selection-reset.json` | `text`, `width`, the three padding transitions and `atOne`/`atThree`/`atZero` rows |
| `frozen hardware-cursor observations replay across MainScreen and AltScreen` | reads `audit-1050/hardware-cursor.json` instead of the historical pi-v1.0.4 bundle | the 13 scenarios, including `main-screen-default-hidden` and `main-screen-toggle` |
| `AltScreen selection replays styled CJK cells across wrapped rows and copies frozen text` (and the other 13 cases in the file) | reads `audit-1050/selection-multiclick.json` instead of the historical pi-v1.0.4 bundle | the 14 selection scenarios; the scenario bodies are identical between the two baselines, so only the binding changed |

The two reader changes matter on their own: a historical bundle observation was standing in for a
v1.3.0 observation of the same scenario names. Both registered v1.3.0 artifacts were compared
scenario by scenario against the historical bundle and are byte-identical below the envelope, so
the correction moves the comparison onto the named baseline without weakening an expectation.

`ProcessTerminalProgramStatusTest.cpp` also stopped copying its expected terminal bytes: the query,
status and clear writes now come from `audit-1050/program-status-detection.json`. That change
exposed the finding below. Its four cases stay in the `-L issue1182` selection even though none of
the rows they compare can be delivered: they are the executed comparison that established the
finding, and excluding them would hide the evidence the batch obligation cites.

### Finding: the frozen stop sequence is not reproduced byte for byte

The registered `audit-1050/program-status-detection.json` scenarios record pi's ordinary `stop()`
writes as `[ESC]7501;state=clear ESC\, ESC[?2004l, ESC[<u, ESC[>4;0m` (`default-reply-before-device-attributes`,
`override-enabled`) and `[ESC[?2004l, ESC[<u, ESC[>4;0m` without the clear
(`default-device-attributes-before-reply`, `override-disabled`). `cch::tui::ProcessTerminal::stop()`
emits the modifyOtherKeys restore only when `modify_other_keys_active` was set
(`src/tui/ProcessTerminal.cpp`, the `modify_other_keys_active` branch before the termios restore).
Under a Kitty-negotiated session that flag is never set, so C++ omits `ESC[>4;0m` and writes a
shorter sequence than the frozen observation.

This is a behavior finding, not a test to relax: no expectation was reduced, and the affected rows
stay unverified. It blocks the eleven rows whose only recorded scenario is one of those three:
`ProcessTerminal`, `ProcessTerminal.start`, `ProcessTerminal.stop`,
`ProcessTerminal.setupStdinBuffer`, `ProcessTerminal.queryAndEnableKittyProtocol`,
`ProcessTerminal.handleKeyboardProtocolNegotiationSequence`,
`ProcessTerminal.setProgramStatus`, `ProcessTerminal.writeProgramStatus`, `Terminal`,
`Terminal.setProgramStatus` and `program-status:detection-override` (eleven row ids, three
scenarios). They are assigned to batches B13 and B16 with the exact scenario ids and this finding as
their implementation obligation. Closing it
needs either the unconditional restore or a registered normalization; this packet does neither.

### Remaining comparison batches (17)

[1182-tui-route-batches.json](1182-tui-route-batches.json) assigns the 913 rows that do not carry a
receipt. The index is a partition: every supported row appears exactly once, no batch may contain a
delivered row, and a batch that loses a required field fails the boundary test.

| Batch | Rows | Caller | Recorded scenario families | Dependency |
| --- | ---: | --- | --- | --- |
| B1 root TUI lifecycle | 113 | `cch::tui::Tui` through `ITui` | E09/E10/E11, E09/E12, E09/E13, E22 | pi-v1.3.0 capture of `screen-state`/`renderer-switch`; cursor-marker implementation |
| B2 regular renderer | 21 | `TuiMainScreen` | E12 regular renderer | renderer-switch capture; cursor-marker implementation |
| B3 alt-screen viewport | 57 | `AltScreen` scroll/scrollbar/search | E13-E15 fullscreen viewport | scroll-view/scrollbar/alt-screen-search capture; #1035 for placement frames |
| B4 transcript selection | 39 | `AltScreen` selection API | selection reset, styled CJK, third click | selection.json capture; public selection reset implementation |
| B5 overlay/wheel routing | 23 | `AltScreen::handle_viewport_input`, `OverlayCompositor` | overlay wheel, viewport | overlay wheel capture; the C++ wheel deferral implementation |
| B6 kitty image placement | 4 | `AltScreen` placement lifecycle | E13-E15 fullscreen viewport | #1035 placement-span emulator observation |
| B7 editor | 90 | `Editor`, `detail::EditorLayout` | E03/E06 editor, kill/yank, undo, E15 | editor-autocomplete + screen-state capture |
| B8 input | 30 | `Input` | E06 Input | input.json capture |
| B9 markdown and math | 51 | `Markdown`, `render_latex` | E08 markdown, E08 latex | new markdown capture (none exists historically) |
| B10 colors | 49 | `ColorUtils`, theme entry points | E17 color parse/style, E17 OKLCH/OKHSL/mix | new color capture (never captured historically) |
| B11 lists | 84 | `SelectList`, `SettingsList` | E07 SelectList, E07 SettingsList | screen-state/mouse-components/fullscreen-settings capture |
| B12 layout and small text | 104 | `detail::LayoutEngine`, `ScrollView`, stacks, `Container`/`Box`/`Text` | E13/E14 layout, E13 stacks, E07 Box/Text/Spacer/TruncatedText, E04 | layout.json + component.json capture; `Box.set_padding_x` implementation; #831 dock frames |
| B13 terminal IO | 110 | `ProcessTerminal`, `StdinBuffer`, `parse_key`, `KeybindingsManager` | E02, E05, E22, fragmented replies, shifted symbols, program-status detection | input/keys capture; the stop-sequence finding; shifted-symbol and fragmented-reply implementations |
| B14 images | 60 | `TerminalImage`, `Image` | E16 image dims/protocol/protocol helpers | image dimension/protocol capture; real emulator placement; #1035 |
| B15 autocomplete and fuzzy | 38 | `AutocompleteProvider`, `fuzzy_match` | E06 autocomplete, E04 fuzzy | autocomplete.json + fuzzy.json capture |
| B16 loader, mouse regions, residuals | 32 | `Loader`, `CancellableLoader`, `MouseRegion`, program-status helpers | E07 Loader/CancellableLoader, E14 MouseRegion | loader/mouse-region capture; shares the B13 stop-sequence finding through `program-status:detection-override` |
| B17 application product entries | 8 | ordinary application Session over a real terminal | native differential, program status, extension editor | #831, #1035, the dual-runtime capture environment, and a real emulator for clipboard/hyperlink |

Each batch entry also names an `obligationOwner`. Where the remaining work has an open owner ticket
it names that ticket (#1035 for B6 and B14, #831 and #1035 for B17); where a C++ implementation gap
has no open implementation ticket, the bounded batch itself is the named owner, which the ticket's
acceptance allows instead of a filed issue. No obligation is left unnamed and none is routed to
#1126.

Each batch entry in the index also carries the concrete normal and discriminating case that must
exist before the batch can compare. Those are the cases the existing tests already implement (for
example `ScrollViewEvidenceTest` for B3, `EditorTest` for B7, `SelectListTest` for B11, or
`NativeTuiDifferentialTest` for B17), so a batch is a capture-and-compare obligation rather than an
unbounded test-writing task. Where the plausible shallow implementation is not separated yet, the
entry names the case that must be added.

## Dated terminal evidence matrix

The selected emulator-dependent outcomes are cursor visibility/composition, image placement,
clipboard, hyperlink and lifecycle. The matrix records the evidence that exists on this host for
each, with the frozen revision, the capture mechanism, the environment, the paired observation, the
discriminator and the artifact. Captures are dated by their own `capturedAt`; the C++ replays were
executed on 2026-11-02 against commit base `6a5093438` plus this packet's pinned inputs.

Host inventory for the dated record: Linux x86-64, glibc 2.44, Wayland session with Ghostty 1.4.0
(`TERM_PROGRAM=ghostty`, `TERM=xterm-256color`), and Kitty 0.49.2, foot 1.28.0 and tmux available.
**No real emulator observation is claimed**: every row below is a virtual-terminal, PTY or frozen
harness observation, and the emulator obligation stays where it is owned.

| Outcome | Frozen observation (revision, capturedAt) | C++ side | Emulator / dimensions / environment | Paired observation | Discriminator | Artifact |
| --- | --- | --- | --- | --- | --- | --- |
| Cursor composition (fake-cursor markers, overlay cursor, hardware-cursor preference) | `audit-1050/cursor-composition.json` @ `71023aec`, 2026-10-19T17:21:56.094Z; `audit-1050/hardware-cursor.json` @ `71023aec`, same date | `AltScreen`/`MainScreen` over `VirtualTerminal`; `CursorMarkerEvidenceTest`, `HardwareCursorEvidenceTest` | none (virtual terminal); 10×1 … 80×24 per scenario; `TERM=xterm-256color`, `PI_HARDWARE_CURSOR` and `TERM_PROGRAM*` pinned | pi's stripped line vs C++ cells/cursor metadata; pi's `showHardwareCursor` preference vs C++ `show_hardware_cursor()` | a frame that hides the cursor regardless of the preference, or a marker pair that survives stripping, fails | `fixtures/pi-v1.3.0/audit-1050/{cursor-composition,hardware-cursor}.json` |
| Image placement (kitty span, overlay inside span, plain terminal control) | `audit-1035/placement-span.json` @ `71023aec`, 2026-10-23T02:58:21.302Z | `AltScreenPlacementSpanTest` over `VirtualTerminal` | none; placement span at 80×24; `TERM_PROGRAM`/`WEZTERM_PANE`/`KITTY_WINDOW_ID` pinned by the capture | pi's placement command trace and damaged row band vs C++ repaint | a repaint that re-emits the whole image instead of the span, or a frame that leaves the footer row unrepainted, fails | `fixtures/pi-v1.3.0/audit-1035/placement-span.json`; emulator obligation owned by [#1035](https://github.com/lanshengzhi/cpp-coding-harness/issues/1035) |
| Image dimensions and protocol detection | `audit-1050/terminal-capabilities.json` @ `71023aec`, 2026-10-19T17:21:56.094Z | `TerminalImageTest`, `ImageTest` | none; detection matrix over pinned environment variables | pi's detected protocol/hyperlink/color booleans vs C++ `detect_image_capabilities` | a Herdr pane that reports no images, or an out-of-order preference, fails | `fixtures/pi-v1.3.0/audit-1050/terminal-capabilities.json` |
| Clipboard (OSC 52 write decision) | `audit-1124/clipboard-osc52.json` @ `71023aec`, 2026-10-22T16:13:35.103Z; dated PTY capture in [1124-dated-terminal-evidence.md](1124-dated-terminal-evidence.md) | real `ProcessTerminal` over a 100×24 PTY; `TerminalSessionCaptureEvidenceTest` | none; host PTY launched from a Herdr terminal environment, `TERM_PROGRAM=herdr` 0.9.5, `COLORTERM=truecolor` | the exact OSC 52 bytes for `pty clipboard evidence` with `PI_CLIPBOARD_OSC52=1`, and none without it | a writer that emits OSC 52 when the opt-in is absent, or that truncates a 4096-byte payload, fails | `fixtures/pi-v1.3.0/audit-1124/clipboard-osc52.json`; **rendering/clipboard effect unverified** — a PTY does not perform the copy |
| Hyperlink (OSC 8 emission and click routing) | `audit-1050/selection-multiclick.json` + `audit-1050/utils-*` hyperlink scenarios @ `71023aec`, 2026-10-19; dated PTY capture in [1124-dated-terminal-evidence.md](1124-dated-terminal-evidence.md) | `SelectionEvidenceTest` click routing over `VirtualTerminal`; real PTY OSC 8 bytes | none; PTY 100×24, `TERM_PROGRAM=herdr` | pi's OSC 8 open/close bytes and click routing vs C++ emitted bytes and `openUrl` decision | a link that swallows the click, or a closure sequence emitted without an opener, fails | `fixtures/pi-v1.3.0/audit-1050/*`; **rendering unverified** — a PTY does not render the link |
| Lifecycle (program status, fullscreen exit, renderer switch, dock footer) | `audit-1123/named-session-program-status.json` @ `71023aec`, 2026-10-20T12:06:05.082Z; `audit-831/dock-footer.json` @ `71023aec`, 2026-10-23T03:21:05.684Z | `ProgramStatusEvidenceTest`, `FullscreenExitOutputTest`, `DockFooterFrameTest` over `VirtualTerminal`/PTY | none; 100×24 (program status), 120×24 (dock) | pi's OSC 7501 lifecycle and dock frame rows vs C++ frames and sequences | a frame that drops the footer row on dock growth, or a status clear that precedes the alternate-restore, fails | `fixtures/pi-v1.3.0/audit-1123/named-session-program-status.json`, `audit-831/dock-footer.json`; emulator obligation owned by [#831](https://github.com/lanshengzhi/cpp-coding-harness/issues/831) |

The emulator-dependent residuals are therefore unchanged and explicitly owned: #831 for the dock
growth frames, #1035 for the placement-span repaint, and batch B17 for the clipboard and hyperlink
rendering outcomes. Setting `TERM_PROGRAM`/`WEZTERM_PANE` or recording PTY bytes is not treated as
proof of a rendered frame or a performed clipboard write anywhere in this matrix.

## Validation

Measured on this ticket's comparison commit (see the delivery record for the exact pinned inputs):

| Command | Result |
| --- | --- |
| `cmake --build --preset vcpkg --target cch_tests_tui` | built; build-phase Parity Architecture Gate PASS |
| `ctest --preset vcpkg -N -L issue1182` | 13 selected: 4 claimed comparison cases, 4 discriminating cases, 4 program-status detection cases strengthened by this packet, and the route-index boundary test |
| `ctest --preset vcpkg -L issue1182 --output-on-failure --output-junit docs/research/pi-v1.3.0/deliveries/1182-revalidation-1187.xml` | 13/13 passed, 0 failed, 0 skipped, on the #1187-merged tree |
| `ctest --preset vcpkg -L issue1182 … deliveries/1182-tui.xml` | the original 13/13 run, retained; the merged-tree rerun above is the pinned artifact and the revalidation entry records why no row, scenario or routing digest changed |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/tui_route_batches.py check` | PASS — 920 supported rows: 7 delivered, 913 batched across 17 batches |
| `python3 tests/evidence/TuiReconciliationTest.py` | 11 tests, all passed |
| `ctest --preset vcpkg -L tui -j1` with `TERM_PROGRAM=herdr` | 1,662 selected, 1,662 passed, 0 failed, 0 skipped |
| `ctest --preset vcpkg -L architecture -j1` | 43 selected, 43 passed |
| `cmake --build --preset vcpkg` then `TERM_PROGRAM=herdr ctest --preset vcpkg -j1` | 4,244 selected, 1 failed, 11 skipped; the one failure passes 3/3 in isolation (see below) |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py build` and `check` | ledger rebuilt (1,464 rows, 67 verified) and PASS — 649 scenarios, 366 named by a row, 56 covered by a referenced artifact, 227 explicit orphans, 0 uncovered |
| `scripts/format-check.sh 6a5093438` | clean |

The receipt pins the exact JUnit bytes, so the selected count, the case names and the scenario
mapping cannot drift silently.

The repository-wide full run is not a requirement for this verification packet — the operator
cancelled the per-ticket full-suite obligation — and the run below is reported because it exists,
not because the packet needs it. It selected 4,244 cases on the merged tree
(`TERM_PROGRAM=herdr`, serial): one failure and eleven skips. The failure,
`Process Terminal atomic stop and default stop detect postdrain EIO on closed master`, is a
load-sensitive PTY case this packet does not touch (`tests/tui/ProcessTerminalTest.cpp` is not a
pinned input); it passed 3/3 when rerun in isolation on the same tree. The skips are the frozen
dual-runtime and external-checkout prerequisites, which supply no acceptance evidence. No timeout,
label or assertion was changed to obtain that result.

## Capability status

Exactly 7 rows move from `upstream-captured` to `implemented` / `verified`, each through the
validated delivery record: `TUI.getShowHardwareCursor`, `TUI.setShowHardwareCursor`,
`ProgramStatus`, `formatProgramStatus`, `Text`, `Text.setPaddingX`, `TuiAltScreen.applySelection`.
All other TUI rows keep their recorded implementation and verification status, their `auditStatus`
history and their named remaining work; 913 of them are named by a batch in the route index.
Membership is unchanged: 920 supported and 5 excluded `cch_tui` rows before and after.

The ledger recorded 60 verified rows before this pass; this pass adds 7 for 67 total and promotes
no other owner's row. It is a reconciliation count, not a completion percentage.

## Remaining boundary

Batches B1–B17 in [1182-tui-route-batches.json](1182-tui-route-batches.json) are the complete
remaining TUI comparison obligation. They must not implement neighboring behavior, must capture any
missing frozen observation instead of deriving it from C++, and must not weaken a normalization.
The stop-sequence finding is part of B13's obligation; the emulator-dependent outcomes stay with
#831, #1035 and B17. `#1044` and `#1126` stay open; closing this verification packet accepts no
adjacent row and no umbrella obligation.

Magpie-only AI access, XDG state roots, strict no-exception behavior, the ordinary application
Session, all explicit AI/platform exclusions and the no-historical-Pike-durable-migration boundary
remain unchanged. SIGKILL evidence does not establish power-loss or multi-process guarantees.
