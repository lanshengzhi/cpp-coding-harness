# #1050 audit: pi-v1.3.0 Linux TUI toolkit capability inventory

## Baseline and scope

- Baseline: `pi-v1.3.0` at `71023aec1546d6d677a65669c5665c0927ce0dd0`.
- Platform: Linux x86-64, glibc 2.44; frozen checkout `/tmp/cch-1050-pi-frozen` (clean, pinned).
- Authority: [ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md), umbrella [spec #1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044), ticket #1050.
- This ticket inventories the public `@earendil-works/pi-tui` surface, captures the changed v1.3.0 behavior, and records C++ status. It changes no production code, no shared evidence infrastructure, and no issue state.
- Every capture below is a real observation from the frozen public API. All rows are `upstream-captured` or `unverified`; none is a `verified` C++ parity claim. Missing C++ implementations are recorded as gaps, not treated as audit blockers or production deliveries.

## Provenance model

The historical `docs/research/tui-v1.0.4-capability-ledger.json` is the identity source. Its 936 rows (173 index exports, 749 members, 14 non-index capabilities) keep their original ids, source endpoints, classification, consumer and tickets. This ticket adds 18 rows: 3 v1.3.0 index exports, 8 new members, and 7 change capabilities/internal helpers. Total: **954 rows** = 176 index exports + 757 members + 14 historical non-index rows + 7 v1.3.0 change rows.

New observations carry `baseline: pi-v1.3.0` and `revision: 71023aec…`. Historical observations keep `historicalBaseline: pi-v1.0.4` and `historicalRevision: 7c10bd43…`; they are never relabelled as v1.3.0 evidence. The comparison routes are explicit in each row:

| Comparison route | Rows | Meaning |
|---|---:|---|
| `v1.3.0-observation` | 49 | A v1.3.0 artifact scenario covers the row or its changed member |
| `member-absent-from-diff` | 455 | Source file changed, but the v1.0.4→v1.3.0 diff never names this member; historical evidence is carried |
| `unchanged-frozen-source` | 410 | Frozen source file is byte-identical between v1.0.4 and v1.3.0 |
| `not-audited` | 35 | Symbol appears in changed hunks but has no behavior-specific capture; deliberately **not** labelled unchanged |
| `not-applicable` | 5 | Approved exclusion |

The 35 `not-audited` rows are the conservative residue, mostly type/member names that occur incidentally in a changed file (for example `Box.paddingX`, `Terminal.write`, `OverlayOptions.width`, the two diagnostic capability rows). They carry a gap rather than a parity claim; the full list is in the ledger and summarized under [Residual gaps](#residual-gaps).

## Evidence artifacts

Capture: `PI_CHECKOUT=/tmp/cch-1050-pi-frozen PI_BASELINE=pi-v1.3.0 /tmp/cch-1050-pi-frozen/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1050/capture-1050.mts`

| Artifact | Scenarios | Bytes | SHA-256 |
|---|---:|---:|---|
| `fixtures/pi-v1.3.0/audit-1050/hardware-cursor.json` | 13 | 27527 | `081e4e8ebd20b5f8665091a6c36a565852dd17076e3506d774c1e044b3508871` |
| `fixtures/pi-v1.3.0/audit-1050/input-standalone.json` | 3 | 3164 | `688cb804e9ab4b8ef406a8a500bc1000bd90fb388e6f264605192d87b6815d78` |
| `fixtures/pi-v1.3.0/audit-1050/selection-multiclick.json` | 14 | 36184 | `ee33e3f9776bdd136099cc54a6636644be17a83720d23db73ddd515aac04a481` |
| `fixtures/pi-v1.3.0/audit-1050/cursor-composition.json` | 8 | 7291 | `6f86fc16da6bfeb72ffb736f3ff4314295d65c76b37c563dc662ecb3c9564590` |
| `fixtures/pi-v1.3.0/audit-1050/padding-and-selection-reset.json` | 4 | 3929 | `134c1297e51205d722e00f7923a1f5598a9d39533ae2aa510dff6d674eba5223` |
| `fixtures/pi-v1.3.0/audit-1050/overlay-routing.json` | 1 | 2257 | `2ad62595a658fe0180345427965b2a36dce372de008377a7773da524dc1326d7` |
| `fixtures/pi-v1.3.0/audit-1050/keys-and-terminal-replies.json` | 4 | 4537 | `f13fa48a1874202288b644c3309b08d9620634e6ac70fe1799a890140df3fbad` |
| `fixtures/pi-v1.3.0/audit-1050/program-status-format.json` | 2 | 2667 | `e41cd21e549ef606e09304b7ade4e420490f91af49b96eb0bbd629c47ea434b8` |
| `fixtures/pi-v1.3.0/audit-1050/program-status-detection.json` | 4 | 4159 | `ceff81b893ef4626cd676cf397353b16fa820454484c4cb92110f08ef6892dc5` |
| `fixtures/pi-v1.3.0/audit-1050/terminal-capabilities.json` | 5 | 3211 | `222da5678d6201fd5ea893425b432f58c3434f93479703e809ac7764814239b0` |

Manifest: `fixtures/pi-v1.3.0/audit-1050/manifest.json`, SHA-256 `3f479083b10886287684afe88a068769ea3989a4020e0559b949c0f937746999` (10 artifacts, 58 scenarios). The capture reuses the historical `hardware-cursor-scenarios.mts`, `input-scenarios.mts` and `selection-scenarios.mts` families against v1.3.0, then adds the changed-behavior scenarios. Absolute checkout paths are normalized to `<pi-checkout>`; the capture asserts baseline, clean source tree, platform, family uniqueness and registry schema before writing.

## Change inventory: v1.0.4 → v1.3.0

The index grew from 173 to 176 exports; none was removed. Fourteen `packages/tui/src` files changed. The table below is the operation/consumer/status summary for every changed capability. The complete per-operation ledger is `docs/research/pi-v1.3.0/1050-toolkit-capability-ledger.json`.

| Area | v1.3.0 operations/members | Frozen observation | C++ status | Required caller / C++ consumer |
|---|---|---|---|---|
| Fake cursor markers | `renderFakeCursor` (new export), `CURSOR_MARKER`, `Editor.render`, `Input.render`, `TUI.showOverlay`, `TuiMainScreen.doRender`, `TuiAltScreen.render`, `resolveFakeCursors`, `FAKE_CURSOR_START/END`, `extractSegments` | `cursor-composition.json`; `input-standalone.json`; `hardware-cursor.json` | partial/missing | Upstream `Editor.render`/`Input.render`/TUI roots; C++ `EditorLayout`/`Input` draw raw reverse video, no marker contract |
| Hardware cursor preference | `TUI.getShowHardwareCursor`, `TUI.setShowHardwareCursor`, Main/Alt-screen cursor positioning | `hardware-cursor.json` (13 scenarios) | implemented | `cch_coding_agent` settings/InteractiveEngine; `tests/tui/HardwareCursorEvidenceTest.cpp` |
| Dynamic horizontal padding | `Box.setPaddingX` (new), `Text.setPaddingX` (new), `Editor.setPaddingX` (existing) | `padding-and-selection-reset.json` transitions (0/1/2/3) | `Box` missing; `Text`/`Editor` implemented | Upstream tool output, custom messages, edit renderer (`outputPad`); C++ `Box` only accepts construction padding, `Text::set_padding_x` has no product caller yet |
| Selection reset | `TuiAltScreen.resetTextSelection` (new), `clearTextSelection`, `lastClick`, `applySelection` | `padding-and-selection-reset.json` reset and third-click scenarios; `selection-multiclick.json` unchanged selection family | reset missing; clear/lastClick partial; apply implemented | Upstream `interactive-mode.ts` calls `resetTextSelection()` before transcript replacement; C++ has no combined public reset |
| Overlay cursor/wheel routing | `TuiAltScreen.handleViewportInput`, `shouldDeferViewportInputToOverlay`, `TUI.showOverlay` | `overlay-routing.json`; `cursor-composition.json` overlay-left-of-cursor | partial | Upstream focused overlay still lets an unhandled wheel scroll the transcript; C++ `AltScreen.cpp:1979` still defers wheel |
| Shifted-symbol keys | `parseKey`, `matchesKey`, Kitty/modifyOtherKeys helpers | `keys-and-terminal-replies.json` shifted-symbol matrix | partial | Upstream keybinding matching; C++ `parse_key`/`matches_key` keep the implicit Shift flag |
| Fragmented terminal-color replies | `StdinBuffer.process`, `flush`, `TERMINAL_REPLY_HEADER_PATTERN`, `isControlString`, `extractCompleteSequences` | `keys-and-terminal-replies.json` fragmented OS C 4 reply, real Ctrl+G, ambiguous Alt-`]` | partial | Upstream `ProcessTerminal` stdin edge; C++ `StdinBuffer.cpp:226` flushes the partial fragment |
| Program status (OSC 7501) | `ProgramStatus` (new), `formatProgramStatus` (new), `Terminal.setProgramStatus`, `ProcessTerminal.setProgramStatus`, `queryAndEnableKittyProtocol`, `setupStdinBuffer`, `writeProgramStatus`, `start`, `stop`, `handleKeyboardProtocolNegotiationSequence` | `program-status-format.json`; `program-status-detection.json` | missing | Upstream `ProgramStatusReporter` in `interactive-mode.ts`; no C++ terminal API or reporter |
| Herdr capabilities | `detectCapabilities`, `detectCapabilitiesFromEnvironment` | `terminal-capabilities.json` | partial (Herdr branch missing) | Upstream `ProcessTerminal.capabilities()` → `hyperlink`/`renderImage`; C++ `TerminalImage.cpp:353-392` has no Herdr branch |

### Fake and hardware cursor behavior

Frozen v1.3.0 wraps each drawn cursor cell in zero-width APC markers:

- `Input.render(10)` for `abc` and focused: `> \u001b_pi:fc\u0007a\u001b_pi:/fc\u0007bc     `; stripping markers yields `> abc     ` at width 10.
- Standalone `Editor.render(10)`: the content line is `hi\u001b_pi:fc\u0007 \u001b_pi:/fc\u0007       `; the marker pair strips to `hi        `; marker count is one.
- Focused `Input` with hardware cursor disabled: reverse video `\x1b[7ma\x1b[27m` plus `\x1b[3G`; the fake cursor is visible.
- Focused `Input` with hardware cursor enabled: no fake cursor cell, only the terminal cursor is shown/positioned; the preference defaults to `false` and an unfocused input keeps its fake cursor.
- Truncating directly after the cursor cell drops the unterminated end marker: no reverse-video cursor is emitted, and the hardware cursor is still positioned.
- An overlay ending directly left of the cursor preserves it: fake mode writes `\x1b[4G`, hardware mode shows the cursor at column 4.

C++ has the preference and cursor positioning but not the representation: `src/tui/Input.cpp:238,260` and `src/tui/EditorLayout.cpp:59,63` insert raw `\x1b[7m…\x1b[27m`; there is no `renderFakeCursor`, no fake-cursor start/end constant, no marker-resolution stage, and no focused-fake-cursor suppression when the hardware cursor is enabled. C++ cursor position is out of band (`Component::cursor_location()`), a valid representation for positioning but not an equivalent for the marker contract.

### Dynamic horizontal padding

Frozen `Box.setPaddingX` and `Text.setPaddingX` invalidate and re-render at the new width, including 0:

| Component | `paddingX=0` | `paddingX=1` | `paddingX=2` | `paddingX=3` |
|---|---|---|---|---|
| `Box` containing `hey`, width 10 | `hey       ` | — | `  hey     ` | `   hey    ` |
| `Text` `hello`, width 10 | `hello     ` | ` hello    ` | — | `   hell` / `   o` |

C++ `Box` accepts `padding_x` only at construction (`src/tui/include/cch/tui/Container.hpp:61`); there is no `set_padding_x`. `Text::set_padding_x` exists (`src/tui/include/cch/tui/Text.hpp:35`, `src/tui/Text.cpp:61`) and `Editor::set_padding_x` exists (`src/tui/Editor.cpp:884`), but no C++ product caller uses `Text::set_padding_x` yet. Upstream callers are `tool-execution.ts`, `custom-message.ts`, `skill-invocation-message.ts`, `compaction-summary-message.ts`, `branch-summary-message.ts`, and `core/tools/renderers/edit.ts`; rendering `outputPad` through the native TUI is a #1051 application row.

### Selection reset

Frozen `resetTextSelection()` drops both the active selection and `lastClick`, so a double-click selection disappears and the next click-drag starts a fresh character selection. A third consecutive click still selects the whole line (the multi-click cycle is otherwise unchanged; the 14 re-captured selection scenarios pass).

C++ `AltScreen::clear_text_selection()` (`src/tui/AltScreen.cpp:1222`) clears the anchor/focus/granularity/gesture but not the click history. `clear_selection_click_history()` exists privately (`src/tui/include/cch/tui/AltScreen.hpp:497`) and runs on start/stop and some mouse paths, but there is no public combined `resetTextSelection()`. This is the one v1.3.0 selection operation missing from the C++ API.

### Overlay cursor and wheel routing

Frozen behavior: with an overlay focused and the wheel turned over the transcript, the overlay receives no wheel input; the transcript scrolls (`viewportTop` 6→5). A page-down key still reaches the overlay. The old `shouldDeferViewportInputToOverlay()` gate remains valid for viewport keys but is no longer used for the unhandled wheel path.

C++ still has `if (should_defer_viewport_input_to_overlay()) return false;` before `route_wheel` (`src/tui/AltScreen.cpp:1979-1981`; helper at `:1004`), so the wheel stays with the overlay. C++ has an overlay compositing path but no v1.3.0 marker-aware comparison, hence `partial` rather than `missing`.

### Shifted-symbol keys

Frozen matrix (`keys-and-terminal-replies.json`):

| Input report | `parseKey` | `matchesKey('+')` | `matchesKey('shift+=')` | `matchesKey('ctrl++')` |
|---|---|---|---|---|
| Kitty `shift+=` | `+` | true | true | false |
| Kitty `ctrl+shift+=` | `ctrl++` | false | false | true |
| modifyOtherKeys `ctrl+shift+=` | `ctrl++` | false | false | true |
| legacy `ctrl+/` | `ctrl+/` | false | false | false |

The v1.3.0 change consumes the implicit Shift when matching the produced symbol while keeping the physical `shift+=` spelling valid. C++ `parse_key`/`matches_key` (`src/tui/Keys.cpp:197,212`) compare the explicit `shift` flag, so the produced-symbol and `ctrl++` identities do not match. The C++ test suite currently covers parse/key-id ordering but not this matrix.

### Fragmented terminal-color replies

Frozen `StdinBuffer` recognizes control-string reply headers (OSC 4/10/11/7501, XTVersion, Kitty graphics). A partial `\x1b]4;1;rgb:eaea/6c` is held past the sequence timeout instead of being emitted; the completed tail is delivered once as `\x1b]4;1;rgb:eaea/6c6c/6d6d\x07`. A real BEL is still delivered as `ctrl+g`; an ambiguous Alt-`]` input is recovered as `\x1b]`, `a`, `x`, `\r` within the timeout.

C++ `StdinBuffer::flush()` (`src/tui/StdinBuffer.cpp:226`) moves the whole buffer to output at the timeout, including partial OSC/APC fragments; the existing test asserts this for an incomplete mouse sequence (`tests/tui/StdinBufferTest.cpp:150-157`). C++ therefore leaks a partial terminal reply into the prompt instead of buffering it.

### Program status (OSC 7501)

Formatting (real frozen output):

- `formatProgramStatus({state:"blocked",app:"pi",kind:"permission",message:"Allow bash?"})` → `\x1b]7501;state=blocked:app=pi:kind=permission:msg=QWxsb3cgYmFzaD8=\x1b\\`.
- `clear` → `\x1b]7501;state=clear\x1b\\`; `working` omits absent fields.
- Control characters in the message are removed/replaced before base64 encoding (`first second [31m third`), and the message is truncated at a UTF-8 character boundary to 2048 bytes (1024 `é` characters; the serialized long-message sequence is 2766 bytes).

Detection and override:

- `start()` writes the keyboard-protocol query, then `\x1b]7501;?\x1b\\`, then DA1 (`\x1b[c`).
- A reply before DA1 enables two queued status writes; `stop()` writes `\x1b]7501;state=clear\x1b\\` before disabling bracketed paste.
- DA1 arriving first suppresses status writes; a late reply is swallowed (no stray input).
- `PI_PROGRAM_STATUS=1` writes statuses without the query; `PI_PROGRAM_STATUS=0` suppresses both query and writes.
- `isProgramStatusReply`/`PROGRAM_STATUS_QUERY` are the public recognition/query pieces.

C++ has no program-status type, formatter, terminal method, query, pending-status state, or stop clear. `Terminal` exposes `set_title` and `set_progress` but no `set_program_status` (`src/tui/include/cch/tui/Terminal.hpp:209,225`). The upstream application consumer is `ProgramStatusReporter` in `packages/coding-agent/src/modes/interactive/interactive-mode.ts`; the native reporter belongs to the #1051 product rows.

### Herdr

Frozen detection checks `TERM_PROGRAM=herdr` before outer-terminal and `KITTY_WINDOW_ID` detection:

| Environment | Images | True color | Hyperlinks |
|---|---|---|---|
| Herdr with `COLORTERM=truecolor` | `kitty` | true | true |
| Herdr with leaked `KITTY_WINDOW_ID` | `kitty` | true | true |
| Herdr with `PI_HYPERLINKS=0` | `kitty` | false | false |
| Kitty control | `kitty` | true | true |
| Unknown terminal | null | true | false |

C++ `detect_image_capabilities` (`src/tui/TerminalImage.cpp:353-392`) checks Kitty, Ghostty, WezTerm and iTerm only; a Herdr pane with no outer-terminal markers reports no images/hyperlinks, and a leaked Kitty variable can steer detection. The native `hyperlink()`/`renderImage` consumers are #1051 application rows.

## Defaults inventory

| Surface | Frozen default | Source |
|---|---|---|
| Hardware cursor | hidden (`getShowHardwareCursor() == false`) | `TUI.getShowHardwareCursor`; `hardware-cursor.json` |
| Focused fake cursor | drawn as reverse video/markers when the hardware cursor is hidden | `Editor.render`, `Input.render`, `TuiMainScreen.doRender` |
| Focused cursor with hardware cursor enabled | fake cursor omitted; terminal cursor shown | `cursor-composition.json` |
| `Box` padding | `paddingX=1`, `paddingY=1` | `Box` constructor |
| `Text` padding | `paddingX=1`, `paddingY=1` | `Text` constructor |
| `Editor` padding | `paddingX=0` | `Editor` default |
| Selection multi-click interval | 500 ms; granularity starts as character | `TuiAltScreen` selection |
| `StdinBuffer` | `timeout=50 ms`, `escapeTimeout=10 ms` | `StdinBufferOptions` |
| Program status | no status until `setProgramStatus`; query sent unless `PI_PROGRAM_STATUS` is set; unsupported terminals no-op | `ProcessTerminal.start` / `program-status.ts` |
| Herdr | `TERM_PROGRAM=herdr` wins over outer-terminal detection; `PI_HYPERLINKS=0` disables hyperlinks and true color | `terminal-image.ts:91` |
| Unknown terminal capabilities | `images: null`, `trueColor` from `COLORTERM`, `hyperlinks: false` | `detectCapabilities` |
| Overlay wheel | unhandled wheel scrolls the transcript even with a focused overlay | `overlay-routing.json` |

## Complete module accounting

The 176 index exports are all present as ledger rows. The 942 supported rows that live under `packages/tui/src` distribute as follows; the 7 product-entry rows (`tuiMode`, `--tui-mode`, `runInteractiveMode`, fullscreen exit/copy/wheel settings, `showHardwareCursor`) live under `packages/coding-agent` and the 5 exclusions are listed below. Member rows are included in the row count.

| Source module | Rows | Index exports | Missing | Partial |
|---|---:|---:|---:|---:|
| `alt-screen-search.ts` | 1 | 0 | 0 | 0 |
| `autocomplete.ts` | 33 | 5 | 0 | 0 |
| `colors.ts` | 48 | 24 | 0 | 0 |
| `components/box.ts` | 18 | 1 | 1 | 1 |
| `components/cancellable-loader.ts` | 4 | 1 | 0 | 0 |
| `components/editor.ts` | 87 | 3 | 0 | 2 |
| `components/h-stack.ts` | 3 | 1 | 0 | 0 |
| `components/image.ts` | 21 | 5 | 0 | 0 |
| `components/input.ts` | 30 | 1 | 0 | 2 |
| `components/loader.ts` | 19 | 2 | 0 | 0 |
| `components/markdown.ts` | 48 | 4 | 0 | 0 |
| `components/mouse-region.ts` | 8 | 2 | 0 | 0 |
| `components/scroll-view.ts` | 45 | 4 | 0 | 0 |
| `components/select-list.ts` | 46 | 5 | 0 | 0 |
| `components/settings-list.ts` | 38 | 3 | 0 | 0 |
| `components/spacer.ts` | 6 | 1 | 0 | 0 |
| `components/text.ts` | 11 | 1 | 0 | 0 |
| `components/truncated-text.ts` | 7 | 1 | 0 | 0 |
| `components/v-stack.ts` | 7 | 5 | 0 | 0 |
| `editor-component.ts` | 13 | 1 | 0 | 0 |
| `fuzzy.ts` | 5 | 3 | 0 | 0 |
| `keybindings.ts` | 27 | 10 | 0 | 0 |
| `keys.ts` | 11 | 10 | 0 | 3 |
| `kill-ring.ts` | 1 | 0 | 0 | 0 |
| `latex.ts` | 3 | 2 | 0 | 0 |
| `layout.ts` | 1 | 0 | 0 | 0 |
| `native-platform.ts` | 6 | 2 | 0 | 0 |
| `oklab.ts` | 1 | 1 | 0 | 0 |
| `program-status.ts` | 3 | 2 | 3 | 0 |
| `stdin-buffer.ts` | 18 | 3 | 0 | 4 |
| `terminal-colors.ts` | 10 | 4 | 0 | 0 |
| `terminal-image.ts` | 40 | 27 | 1 | 1 |
| `terminal.ts` | 54 | 2 | 5 | 5 |
| `tui-alt-screen.ts` | 124 | 2 | 1 | 6 |
| `tui-main-screen.ts` | 21 | 2 | 0 | 2 |
| `tui.ts` | 115 | 26 | 3 | 3 |
| `undo-stack.ts` | 1 | 0 | 0 | 0 |
| `utils.ts` | 7 | 6 | 0 | 1 |
| `wheel-scroll.ts` | 1 | 1 | 0 | 0 |

This covers rendering/lifecycle/focus/layout (`tui*.ts`, components, overlays, scroll views, stacks), input/Unicode/ANSI helpers (`input`, `editor`, `keys`, `stdin-buffer`, `utils`, `colors`), Markdown/math (`components/markdown.ts`, `latex.ts`), autocomplete/keybindings/settings/loaders, and terminal queries/clipboard/image (`terminal.ts`, `terminal-image.ts`, `terminal-colors.ts`, `native-platform.ts`, `components/image.ts`). Every row carries `cpp.interface`, `cpp.consumer`, `cpp.tests`, membership, implementation, verification and `comparisonCase`; consumers, source endpoints, test pointers and gaps are per row in the ledger.

Terminal query/clipboard/image rows are implemented at the reusable layer: cell-size query/refinement (`getCellDimensions`, `setCellDimensions`), terminal color-scheme reporting (`parseTerminalColorSchemeReport`, `getTerminalColorMode`), Linux clipboard (`getNativeClipboard`, `NativeClipboard`), and the Kitty/iTerm2 image helpers (`allocateImageId`, `encodeKitty`, `encodeITerm2`, dimension probing, `renderImage`, `imageFallback`, `hyperlink`). The only changed row in this group is Herdr detection (`partial`); the image/clipboard consumers in the application remain #1051 rows.

### Integration dependencies and ownership

- **One interactive output authority.** The reusable chain is `cch_tui::ProcessTerminal` (input/capability edge) → `cch_tui::MainScreen`/`AltScreen` (composition, cursor, overlay, selection) → the application `InteractiveView`/`InteractiveEngine` (one renderer/authority). The TUI rows stay `cch_tui`; the application renderer is not duplicated in this audit.
- **Program status** splits into a reusable terminal capability (`ProgramStatus`, `formatProgramStatus`, `Terminal.setProgramStatus`) and an application reporter (`ProgramStatusReporter`, #1051 product row). Neither side may own both.
- **Dynamic padding** splits into the reusable component setter (`Box`/`Text`) and the application `outputPad` call sites (#1051). `Editor.set_padding_x` is already reusable and consumed by the application editor path.
- **Cursor markers** are a TUI rendering contract; custom application components continue to provide `Component::cursor_location()` rather than a second marker implementation.
- **Herdr** belongs to TUI capability detection; application `hyperlink`/`renderImage` consumers inherit the result. No new application detector is introduced.
- **Shared identities with #1051.** Product rows keep their historical ids (`product.tuiMode`, `product.--tui-mode`, `product.runInteractiveMode`, `product.fullscreenExitOutput`, `product.fullscreenCopyOnSelect`, `product.fullscreenWheelScrollLines`, `product.showHardwareCursor`) and are not relocated or duplicated; the application audit can cross-reference them by id.

## Representation-specific equivalents and exclusions

- **Representation-specific equivalents (348 historical rows).** The v1.0.4 ledger's `cpp-representation` rows remain supported and keep their `cpp_contract` boundary. Examples: cursor position is out of band via `Component::cursor_location()` instead of `CURSOR_MARKER`; stack/layout/scrolling geometry is represented by native components rather than the upstream helper objects; type-only exports are carried as C++ types. These are not new exclusions and are not relabelled as v1.3.0 findings.
- **Approved exclusions (5).** `Marked`, `Token`, `Tokens` (unconsumable parser/runtime objects; Markdown/math covered through `Markdown`/`renderLatex`), `isAppleTerminalSession` (macOS-only), and `internal.native-modifiers` (platform-exclusive synthesis, no Linux consumer). All five keep `membership: excluded`, `verification: not-applicable`, no gaps, and the historical reason in `boundary`.
- No Linux-usable capability was newly excluded by this ticket.

## Verification and evidence status

- The evidence checker accepts the manifest: `python3 scripts/evidence/audit_evidence.py check --manifest fixtures/pi-v1.3.0/audit-1050/manifest.json` exits 0 and prints the registry record above. Until the serial merger applies that record, the bundle remains `upstream-captured`, not `verified`.
- Row verification: **49 upstream-captured, 900 unverified, 0 verified, 5 not-applicable**. The 900 unverified rows are honest: 865 have a historical comparison route and 35 have a `not-audited` gap. No C++ differential capture was run for this audit, and no real terminal-emulator evidence exists for the C++ side: every native status is source/test inventory, not an emulator observation. That remains a visible verification gap, not an unconditional pass or skip.
- Functional status: **905 implemented, 30 partial, 14 missing** among supported rows (19 missing including the 5 exclusions), 44 supported rows with a functional gap.
- Frozen upstream suites were run against the same checkout to corroborate the captured behavior: 20 files, **873 tests passed** (`editor`, `input`, `keys`, `stdin-buffer`, `terminal`, `terminal-image`, `program-status`, `tui-render`, `tui-alt-screen`, `overlay-options`, `native-clipboard-linux`, `markdown`, `latex`, `layout`, `wheel-scroll`, `visible-width`, `truncate-to-width`, `wrap-ansi`, `colors`, `mouse-components`). The checkout remained clean at `71023aec…`.
- Ledger validator: `python3 fixtures/pi-v1.3.0/audit-1050/validate-ledger.py --pi-checkout /tmp/cch-1050-pi-frozen --ledger docs/research/pi-v1.3.0/1050-toolkit-capability-ledger.json` passes (954 rows, 176 index exports, 5 exclusions, 18 additions, 10 artifacts; every v1.3.0 evidence scenario exists).

## Acceptance mapping

1. **All Linux-usable capabilities/defaults/callers.** 176 index exports + 757 members + 14 historical non-index rows + 7 v1.3.0 change rows are accounted; defaults table above; every row has a consumer (or an explicit `none` with the missing caller named); additions since v1.0.4 are the 3 exports/8 members/7 change rows; representation equivalents and 5 platform/parser exclusions are named.
2. **Frozen public scenarios.** Fake/hardware cursor markers and standalone components (`cursor-composition.json`, `input-standalone.json`, reused `hardware-cursor.json`); dynamic padding and selection reset (`padding-and-selection-reset.json`); overlay cursor/wheel (`overlay-routing.json`, `cursor-composition.json`); shifted-symbol keys and fragmented color replies (`keys-and-terminal-replies.json`); reused `selection-multiclick.json` for the unchanged selection family.
3. **Inventory domains.** Program status (format/detection/override) and Herdr are detailed above; rendering/lifecycle/focus/layout, input/Unicode/ANSI/Markdown/math and terminal queries/clipboard/image are covered by the module table and per-row consumers; reusable TUI rows are `cch_tui`, while product-entry rows (`tuiMode`, `--tui-mode`, `runInteractiveMode`, `showHardwareCursor`, fullscreen settings) are `frontend_*`/`cch_coding_agent` responsibilities.
4. **Provenance.** New observations bind to `pi-v1.3.0`/`71023aec…`; historical rows retain `pi-v1.0.4`/`7c10bd43…`; unchanged rows use the unchanged-source or member-absent-from-diff route; 35 ambiguous rows are explicitly not claimed as unchanged.

## Residual gaps

The 44 supported rows with a functional gap group as follows (full details in the ledger):

- **Program status (missing, 8 rows):** `ProgramStatus`, `formatProgramStatus`, `Terminal.setProgramStatus`, `ProcessTerminal.setProgramStatus`, `queryAndEnableKittyProtocol`, `setupStdinBuffer`, `writeProgramStatus`, plus the detection/override capability row; `ProcessTerminal.start/stop/handleKeyboardProtocolNegotiationSequence` are partial.
- **Fake cursor markers (missing/partial, 6 rows):** `renderFakeCursor`, marker constants, `resolveFakeCursors`, `Editor.render`, `Input.render`, `CURSOR_MARKER`; `TuiMainScreen.doRender`, `TuiAltScreen.render`, `TUI.showOverlay` and `extractSegments` are partial.
- **Dynamic padding (1 row):** `Box.setPaddingX` missing; `Box` export partial. `Text.setPaddingX` is implemented but unconsumed.
- **Selection reset (3 rows):** `resetTextSelection` missing; `clearTextSelection` and `lastClick` partial.
- **Overlay wheel (2 rows):** `handleViewportInput` and `shouldDeferViewportInputToOverlay` partial.
- **Shifted keys (2 rows + capability):** `parseKey`, `matchesKey`, `cch_tui:keys:shifted-symbol-matching` partial.
- **Fragmented replies (2 rows + capability):** `StdinBuffer.process`, `flush`, `cch_tui:stdin-buffer:fragmented-terminal-replies` partial.
- **Herdr (2 rows):** `detectCapabilities` and `detectCapabilitiesFromEnvironment` partial/missing.

The 35 `not-audited` rows are listed in the ledger with `comparisonRoute: not-audited`; they are the rows whose symbol text occurs in a changed hunk but whose behavior was not separately captured. They are deliberately excluded from unchanged-behavior claims. The 900 `unverified` rows have no C++ differential capture; `verified` remains zero by design for this audit.

## Consumers and tests affected

| Consumer seam | C++ implementation | Existing tests |
|---|---|---|
| Prompt/editor cursor rendering | `src/tui/Input.cpp`, `src/tui/EditorLayout.cpp`, `src/tui/Editor.cpp` | `tests/tui/EditorTest.cpp`, `tests/tui/InputTest.cpp` |
| Hardware cursor preference/composition | `src/tui/TuiMainScreen.cpp`, `src/tui/AltScreen.cpp` | `tests/tui/HardwareCursorEvidenceTest.cpp` |
| Fullscreen selection | `src/tui/AltScreen.cpp` | `tests/tui/SelectionEvidenceTest.cpp`, `tests/tui/AltScreenTest.cpp` |
| Overlay/wheel input | `src/tui/AltScreen.cpp` (`handle_wheel_report`, `route_wheel`) | `tests/tui/AltScreenTest.cpp`, `tests/tui/OverlayTest.cpp` |
| Key parsing/matching | `src/tui/Keys.cpp` | `tests/tui/KeysTest.cpp`, `tests/tui/KeysKittyTextTest.cpp`, `tests/tui/KeysLegacyTest.cpp` |
| Terminal input buffering | `src/tui/StdinBuffer.cpp` | `tests/tui/StdinBufferTest.cpp` |
| Terminal capabilities/image/hyperlink | `src/tui/TerminalImage.cpp`, `src/tui/TerminalImageState.cpp` | `tests/tui/TerminalImageTest.cpp`, `tests/tui/ProcessTerminalTest.cpp` |
| Program status / terminal interface | `src/tui/include/cch/tui/Terminal.hpp`, `src/tui/ProcessTerminal.cpp` | none for OSC 7501 |
| Dynamic component padding | `src/tui/include/cch/tui/Container.hpp` (Box), `src/tui/include/cch/tui/Text.hpp` | `tests/tui/ContainerTest.cpp` |

## Included, excluded, unresolved

- Included: the 10 capture artifacts/58 scenarios, capture harness and child process, the 954-row ledger, the human report, and the ticket-owned validator.
- Excluded by authority: no production fixes or migrations; no shared registry edit (the record is a handoff request); no issue mutation; no macOS/parser-object capabilities.
- Unresolved: 44 functional gaps above; 35 not-audited rows; no C++ differential/verified capture; the shared registry record is pending serial merger.

## Validation commands

- Capture: `PI_CHECKOUT=/tmp/cch-1050-pi-frozen PI_BASELINE=pi-v1.3.0 /tmp/cch-1050-pi-frozen/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1050/capture-1050.mts` — 10 artifacts, 58 scenarios, exit 0.
- Evidence check: `python3 scripts/evidence/audit_evidence.py check --manifest fixtures/pi-v1.3.0/audit-1050/manifest.json` — exit 0.
- Ledger build: `python3 fixtures/pi-v1.3.0/audit-1050/build-ledger.py --pi-checkout /tmp/cch-1050-pi-frozen --output docs/research/pi-v1.3.0/1050-toolkit-capability-ledger.json` — 954 rows.
- Ledger validation: `python3 fixtures/pi-v1.3.0/audit-1050/validate-ledger.py --pi-checkout /tmp/cch-1050-pi-frozen --ledger docs/research/pi-v1.3.0/1050-toolkit-capability-ledger.json` — passed.
- Frozen upstream tests: `node --test --test-reporter=spec test/editor.test.ts … test/mouse-components.test.ts` (18 files) — 873 passed, 0 failed.
- Frozen checkout cleanliness: `git -C /tmp/cch-1050-pi-frozen status --short` — empty; `HEAD` at `71023aec…`.

## Recommended next steps

1. Apply the registry record through the #1046 serial merger so the bundle moves from `upstream-captured` to `verified` (the C++ rows remain unverified until a native comparison exists).
2. In a #1058 implementation slice, start with the independent TUI gaps: `renderFakeCursor`/marker resolution and hardware-cursor suppression; `Box::set_padding_x`; `AltScreen::reset_text_selection`; shifted-symbol `parse_key`/`matches_key`; fragmented terminal-reply buffering; Herdr detection; then the overlay-wheel routing change.
3. Implement OSC 7501 as a terminal-layer capability plus a `cch_coding_agent` program-status reporter; the native reporter is a #1051 product row, not a TUI audit deliverable.
4. Add a C++ differential capture for the 49 upstream-captured rows before claiming `verified`, and use the 35 `not-audited` rows as the review queue for the changed-file triage.

## Pointers

- Ledger: `docs/research/pi-v1.3.0/1050-toolkit-capability-ledger.json`.
- Evidence manifest: `fixtures/pi-v1.3.0/audit-1050/manifest.json`.
- Capture/build/validate: `fixtures/pi-v1.3.0/audit-1050/{capture-1050.mts,scenarios-1050.mts,program-status-child.mts,build-ledger.py,validate-ledger.py}`.
- Historical identity/evidence: `docs/research/tui-v1.0.4-capability-ledger.json`, `fixtures/pi-tui/bundles/pi-v1.0.4/manifest.json`.
- Handoff and shared registry request: `/tmp/cch-1044-context/1050-handoff.json`.
