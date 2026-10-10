# TUI v1.0.4 capability and consumer ledger

- **Baseline**: `pi-v1.0.4` at `7c10bd4337495ee613f2224843ecdf349b80d1df`
- **Authority**: [ADR 0067](../adr/0067-align-the-linux-tui-toolkit-and-native-tui-with-pi-v1-0-4.md), spec #946, ticket #948
- **Machine ledger**: [`tui-v1.0.4-capability-ledger.json`](tui-v1.0.4-capability-ledger.json)
- **Method**: static export enumeration + class/interface public signature scrape + product entry audit

## Stats

- Index exports accounted: **173**
- Export rows: **173**; member/operation rows: **749**; total rows: **936**
- Classifications: `{'approved-exclusion': 5, 'cpp-representation': 348, 'included': 583}`
- No included row is classified Deferred or assigned only to close-out.

## Cross-references to older issues

| Issue | Disposition | Related tickets |
| --- | --- | --- |
| #831 Native TUI dock-growth frames drop footer rows | preserve footer-preservation regression cases; dock-only remedy superseded by #978/#983/#987 single-output-path migration | #978, #983, #987, #998 |
| #811 Align Native TUI editor viewport composition for wrapped and narrow input | rechecked: later comment reports original dual-runtime repro stopped reproducing after theme/editor text-style alignment; viewport/padding work still owned by #969 without assuming #811 blocks | #969 |
| #809 Align Native TUI settings selector supported rows while retaining pi-only omissions | old settings-omission rule superseded only for capabilities included by ADR0067/#946; SettingsList/fullscreen settings owned by #968/#1020 | #968, #1020 |
| #749 refactor(cch_tui): deepen editor layout seam for staged simplification | reuse landed layout/renderer seams; do not repeat umbrella work | #969, #977, #983 |
| #830 refactor(cch_tui): name the frame commit/rollback protocol inside RenderPipeline | related renderer commit/rollback cleanup; recorded without treating as required blocker for #946 graph | #986, #987 |

These issues are not mutated by #948.

## Approved exclusions

| Id | Reason |
| --- | --- |
| `Marked` | ADR0067/spec exclude unconsumable parser-object exports |
| `Token` | ADR0067/spec exclude unconsumable parser-object exports |
| `Tokens` | ADR0067/spec exclude unconsumable parser-object exports |
| `isAppleTerminalSession` | Native macOS helper excluded; Linux clipboard handled by #992/#993 |
| `internal.native-modifiers` | platform-exclusive synthesis excluded |

## Product entry points

| Id | Owning package | Consumer | Tickets | Evidence |
| --- | --- | --- | --- | --- |
| `product.tuiMode` | frontend_cli/frontend_tui | CLI parse + settings + interactive mode | #1017, #1020, #1024 | E20 tuiMode/default/CLI precedence |
| `product.--tui-mode` | frontend_cli | AsyncCliRuntime / interactive startup | #1017, #1024 | E20 CLI overrides settings |
| `product.runInteractiveMode` | frontend_tui | run_interactive_mode(Terminal&, InteractiveSessionRun) | #977, #987, #1017, #1018, #1019, #1024 | E20/E21 interactive lifecycle |
| `product.fullscreenExitOutput` | frontend_tui | fullscreen stop/exit policy | #1019, #1020 | E20 transcript vs resume-hint exit output |
| `product.fullscreenCopyOnSelect` | frontend_tui | fullscreen selection copy | #1006, #1020 | E15 copy-on-select false/true |
| `product.fullscreenWheelScrollLines` | frontend_tui | wheel scroll | #1004, #1020 | E14 configurable wheel lines |
| `product.showHardwareCursor` | frontend_tui + cch_tui | renderer construction / IME | #980, #1021 | E10/E1021 hardware cursor visibility |

## Index export summary

Each index export is classified and assigned. Public class/interface operations are expanded in the JSON ledger (`public_operations` / member rows).

| Export | Kind | Classification | Owning package | Tickets | Evidence | C++ contract |
| --- | --- | --- | --- | --- | --- | --- |
| `AutocompleteItem` | interface (3 ops) | cpp-representation | cch_tui | #963, #964, #965, #970 | E06 autocomplete | CombinedAutocompleteProvider + provider contracts |
| `AutocompleteProvider` | interface (10 ops) | cpp-representation | cch_tui | #963, #964, #965, #970 | E06 autocomplete | CombinedAutocompleteProvider + provider contracts |
| `AutocompleteSuggestions` | interface (2 ops) | cpp-representation | cch_tui | #963, #964, #965, #970 | E06 autocomplete | CombinedAutocompleteProvider + provider contracts |
| `Box` | class (16 ops) | included | cch_tui | #966 | E07 Box mutation/cache | Box |
| `CURSOR_MARKER` | const | cpp-representation | cch_tui | #980, #947 | E10 out-of-band cursor metadata vs marker representation | out-of-band cursor metadata (no CURSOR_MARKER string requirement) |
| `CancellableLoader` | class (3 ops) | included | cch_tui | #979 | E07 CancellableLoader | CancellableLoader |
| `CellDimensions` | interface (2 ops) | cpp-representation | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `Color` | type | cpp-representation | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `ColorMixSpace` | type | cpp-representation | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `CombinedAutocompleteProvider` | class (9 ops) | included | cch_tui | #963, #964, #965, #970 | E06 autocomplete | CombinedAutocompleteProvider + provider contracts |
| `Component` | interface (5 ops) | cpp-representation | cch_tui | #952, #977, #978, #980, #981, #982, #1016 | E09/E10/E11 common TUI lifecycle | TUI/Container/Component/Overlay/listeners |
| `Container` | class (8 ops) | included | cch_tui | #952, #977, #978, #980, #981, #982, #1016 | E09/E10/E11 common TUI lifecycle | TUI/Container/Component/Overlay/listeners |
| `DefaultTextStyle` | interface (6 ops) | cpp-representation | cch_tui | #971, #972, #976 | E08 Markdown/tables/math integration | Markdown |
| `Editor` | class (80 ops) | included | cch_tui | #950, #953, #954, #955, #960, #961, #969, #970 | E03/E06 editor | Editor options/theme/ops |
| `EditorComponent` | interface (12 ops) | cpp-representation | cch_tui + frontend_tui extension host | #1013, #1014, #1015 | E19 extension editor lifecycle | EditorComponent replaceable contract |
| `EditorOptions` | interface (2 ops) | cpp-representation | cch_tui | #950, #953, #954, #955, #960, #961, #969, #970 | E03/E06 editor | Editor options/theme/ops |
| `EditorTheme` | interface (2 ops) | cpp-representation | cch_tui | #950, #953, #954, #955, #960, #961, #969, #970 | E03/E06 editor | Editor options/theme/ops |
| `Focusable` | interface (1 ops) | cpp-representation | cch_tui | #952, #977, #978, #980, #981, #982, #1016 | E09/E10/E11 common TUI lifecycle | TUI/Container/Component/Overlay/listeners |
| `FuzzyMatch` | interface (2 ops) | cpp-representation | cch_tui | #958 | E04 fuzzy offsets | fuzzyMatch/fuzzyFilter with original UTF-8 offsets |
| `HStack` | class (2 ops) | included | cch_tui | #999 | E13 HStack | HStack |
| `Image` | class (11 ops) | included | cch_tui + frontend transcoder injection | #994, #995, #996 | E16 Image | Image + setImageTranscoder injection |
| `ImageDimensions` | interface (2 ops) | cpp-representation | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `ImageOptions` | interface (4 ops) | cpp-representation | cch_tui + frontend transcoder injection | #994, #995, #996 | E16 Image | Image + setImageTranscoder injection |
| `ImageProtocol` | type | cpp-representation | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `ImageRenderOptions` | interface (5 ops) | cpp-representation | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `ImageTheme` | interface (1 ops) | cpp-representation | cch_tui + frontend transcoder injection | #994, #995, #996 | E16 Image | Image + setImageTranscoder injection |
| `ImageTranscoder` | type | cpp-representation | cch_tui + frontend transcoder injection | #994, #995, #996 | E16 Image | Image + setImageTranscoder injection |
| `IndexedColor` | interface (2 ops) | cpp-representation | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `Input` | class (29 ops) | included | cch_tui | #950, #953, #962 | E06 Input | Input presentation/LF/paste |
| `Key` | const | included | cch_tui | #950, #951 | E02 Kitty text vs shortcut identity; legacy/modifyOtherKeys/dup | Keys parse/match/printable helpers with per-terminal state |
| `KeyEventType` | type | cpp-representation | cch_tui | #950, #951 | E02 Kitty text vs shortcut identity; legacy/modifyOtherKeys/dup | Keys parse/match/printable helpers with per-terminal state |
| `KeyId` | type | cpp-representation | cch_tui | #950, #951 | E02 Kitty text vs shortcut identity; legacy/modifyOtherKeys/dup | Keys parse/match/printable helpers with per-terminal state |
| `Keybinding` | type | cpp-representation | cch_tui + frontend consumers | #959 | E05 binding context convergence | shared/local KeybindingsManager snapshots |
| `KeybindingConflict` | interface (2 ops) | cpp-representation | cch_tui + frontend consumers | #959 | E05 binding context convergence | shared/local KeybindingsManager snapshots |
| `KeybindingDefinition` | interface (2 ops) | cpp-representation | cch_tui + frontend consumers | #959 | E05 binding context convergence | shared/local KeybindingsManager snapshots |
| `KeybindingDefinitions` | type | cpp-representation | cch_tui + frontend consumers | #959 | E05 binding context convergence | shared/local KeybindingsManager snapshots |
| `Keybindings` | interface | cpp-representation | cch_tui + frontend consumers | #959 | E05 binding context convergence | shared/local KeybindingsManager snapshots |
| `KeybindingsConfig` | type | cpp-representation | cch_tui + frontend consumers | #959 | E05 binding context convergence | shared/local KeybindingsManager snapshots |
| `KeybindingsManager` | class (13 ops) | included | cch_tui + frontend consumers | #959 | E05 binding context convergence | shared/local KeybindingsManager snapshots |
| `Loader` | class (15 ops) | included | cch_tui | #979 | E07 Loader | Loader |
| `LoaderIndicatorOptions` | interface (2 ops) | cpp-representation | cch_tui | #979 | E07 Loader | Loader |
| `Markdown` | class (18 ops) | included | cch_tui | #971, #972, #976 | E08 Markdown/tables/math integration | Markdown |
| `MarkdownOptions` | interface (4 ops) | cpp-representation | cch_tui | #971, #972, #976 | E08 Markdown/tables/math integration | Markdown |
| `MarkdownTheme` | interface (16 ops) | cpp-representation | cch_tui | #971, #972, #976 | E08 Markdown/tables/math integration | Markdown |
| `Marked` | excluded-reexport | approved-exclusion | n/a | #946-out-of-scope | exclusion: Marked/Token/Tokens not emulated for C++ callers | none — Markdown/math behavior covered via Markdown/renderLatex seams |
| `MouseRegion` | class (6 ops) | included | cch_tui | #1002, #1003 | E14 MouseRegion | MouseRegion |
| `MouseRegionHandler` | type | cpp-representation | cch_tui | #1002, #1003 | E14 MouseRegion | MouseRegion |
| `NativeClipboard` | interface (4 ops) | included | cch_tui (+ frontend_tui adapter) | #992, #993 | E18 unavailable/empty/failed Linux clipboard outcomes | injected NativeClipboard-equivalent capability interface |
| `OkhslChannels` | interface (3 ops) | cpp-representation | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `OklchChannels` | interface (3 ops) | cpp-representation | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `OklchColorValue` | interface (4 ops) | cpp-representation | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `OverlayAnchor` | type | cpp-representation | cch_tui | #952, #977, #978, #980, #981, #982, #1016 | E09/E10/E11 common TUI lifecycle | TUI/Container/Component/Overlay/listeners |
| `OverlayBounds` | interface (4 ops) | cpp-representation | cch_tui | #952, #977, #978, #980, #981, #982, #1016 | E09/E10/E11 common TUI lifecycle | TUI/Container/Component/Overlay/listeners |
| `OverlayHandle` | interface (7 ops) | cpp-representation | cch_tui | #952, #977, #978, #980, #981, #982, #1016 | E09/E10/E11 common TUI lifecycle | TUI/Container/Component/Overlay/listeners |
| `OverlayMargin` | interface (4 ops) | cpp-representation | cch_tui | #952, #977, #978, #980, #981, #982, #1016 | E09/E10/E11 common TUI lifecycle | TUI/Container/Component/Overlay/listeners |
| `OverlayOptions` | interface (11 ops) | cpp-representation | cch_tui | #952, #977, #978, #980, #981, #982, #1016 | E09/E10/E11 common TUI lifecycle | TUI/Container/Component/Overlay/listeners |
| `OverlayUnfocusOptions` | interface (1 ops) | cpp-representation | cch_tui | #952, #977, #978, #980, #981, #982, #1016 | E09/E10/E11 common TUI lifecycle | TUI/Container/Component/Overlay/listeners |
| `ProcessTerminal` | class (34 ops) | included | cch_tui | #949, #986, #992 | E02/E09/E12 terminal IO | Terminal interface + ProcessTerminal |
| `RenderLatexOptions` | interface (1 ops) | cpp-representation | cch_tui | #973, #974, #975, #976 | E08 latex | renderLatex reusable capability |
| `RgbColor` | interface (3 ops) | cpp-representation | cch_tui + frontend_tui Theme | #990, #991 | E17 terminal color queries | parseTerminalColorSchemeReport + TerminalColors values |
| `RgbColorValue` | interface (4 ops) | cpp-representation | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `ScrollView` | class (32 ops) | included | cch_tui | #1000, #1001, #1004, #1005 | E13/E14 ScrollView | ScrollView |
| `ScrollViewOptions` | interface (8 ops) | cpp-representation | cch_tui | #1000, #1001, #1004, #1005 | E13/E14 ScrollView | ScrollView |
| `ScrollViewScrollToOptions` | interface (1 ops) | cpp-representation | cch_tui | #1000, #1001, #1004, #1005 | E13/E14 ScrollView | ScrollView |
| `ScrollViewScrollbar` | type | cpp-representation | cch_tui | #1000, #1001, #1004, #1005 | E13/E14 ScrollView | ScrollView |
| `SelectItem` | interface (3 ops) | cpp-representation | cch_tui + frontend selectors | #967 | E07 SelectList | SelectList; product chrome moves to composition |
| `SelectList` | class (25 ops) | included | cch_tui + frontend selectors | #967 | E07 SelectList | SelectList; product chrome moves to composition |
| `SelectListLayoutOptions` | interface (3 ops) | cpp-representation | cch_tui + frontend selectors | #967 | E07 SelectList | SelectList; product chrome moves to composition |
| `SelectListTheme` | interface (5 ops) | cpp-representation | cch_tui + frontend selectors | #967 | E07 SelectList | SelectList; product chrome moves to composition |
| `SelectListTruncatePrimaryContext` | interface (5 ops) | cpp-representation | cch_tui + frontend selectors | #967 | E07 SelectList | SelectList; product chrome moves to composition |
| `SettingItem` | interface (7 ops) | cpp-representation | cch_tui + frontend settings host | #968, #1020 | E07 SettingsList | SettingsList submenu/selection |
| `SettingsList` | class (23 ops) | included | cch_tui + frontend settings host | #968, #1020 | E07 SettingsList | SettingsList submenu/selection |
| `SettingsListTheme` | interface (5 ops) | cpp-representation | cch_tui + frontend settings host | #968, #1020 | E07 SettingsList | SettingsList submenu/selection |
| `SizeValue` | type | cpp-representation | cch_tui | #952, #977, #978, #980, #981, #982, #1016 | E09/E10/E11 common TUI lifecycle | TUI/Container/Component/Overlay/listeners |
| `SlashCommand` | interface (4 ops) | cpp-representation | cch_tui | #963, #964, #965, #970 | E06 autocomplete | CombinedAutocompleteProvider + provider contracts |
| `Spacer` | class (5 ops) | included | cch_tui | #966 | E07 Spacer | Spacer |
| `StackChild` | type | cpp-representation | cch_tui | #998, #999 | E13 VStack | VStack/StackEntry |
| `StackEntry` | type | cpp-representation | cch_tui | #998, #999 | E13 VStack | VStack/StackEntry |
| `StackEntryOptions` | type | cpp-representation | cch_tui | #998, #999 | E13 VStack | VStack/StackEntry |
| `StackOptions` | type | cpp-representation | cch_tui | #998, #999 | E13 VStack | VStack/StackEntry |
| `StdinBuffer` | class (14 ops) | included | cch_tui | #949, #952, #953, #954 | E02 framing/listener/paste buffering | StdinBuffer-equivalent reusable buffering |
| `StdinBufferEventMap` | type | cpp-representation | cch_tui | #949, #952, #953, #954 | E02 framing/listener/paste buffering | StdinBuffer-equivalent reusable buffering |
| `StdinBufferOptions` | type | cpp-representation | cch_tui | #949, #952, #953, #954 | E02 framing/listener/paste buffering | StdinBuffer-equivalent reusable buffering |
| `TUI` | interface (27 ops) | cpp-representation | cch_tui | #952, #977, #978, #980, #981, #982, #1016 | E09/E10/E11 common TUI lifecycle | TUI/Container/Component/Overlay/listeners |
| `TUI_KEYBINDINGS` | const | included | cch_tui + frontend consumers | #959 | E05 binding context convergence | shared/local KeybindingsManager snapshots |
| `Terminal` | interface (12 ops) | cpp-representation | cch_tui | #949, #986, #992 | E02/E09/E12 terminal IO | Terminal interface + ProcessTerminal |
| `TerminalCapabilities` | interface (3 ops) | cpp-representation | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `TerminalColorMode` | type | cpp-representation | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `TerminalColorScheme` | type | cpp-representation | cch_tui + frontend_tui Theme | #990, #991 | E17 terminal color queries | parseTerminalColorSchemeReport + TerminalColors values |
| `TerminalColors` | interface (3 ops) | cpp-representation | cch_tui + frontend_tui Theme | #990, #991 | E17 terminal color queries | parseTerminalColorSchemeReport + TerminalColors values |
| `Text` | class (9 ops) | included | cch_tui | #966 | E07 Text | Text render/setText |
| `TextAttributes` | interface (6 ops) | cpp-representation | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `TextStyle` | interface (2 ops) | cpp-representation | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `Token` | excluded-reexport | approved-exclusion | n/a | #946-out-of-scope | exclusion: Marked/Token/Tokens not emulated for C++ callers | none — Markdown/math behavior covered via Markdown/renderLatex seams |
| `Tokens` | excluded-reexport | approved-exclusion | n/a | #946-out-of-scope | exclusion: Marked/Token/Tokens not emulated for C++ callers | none — Markdown/math behavior covered via Markdown/renderLatex seams |
| `TruncatedText` | class (6 ops) | included | cch_tui | #966, #957 | E07 TruncatedText | TruncatedText |
| `TuiAltScreen` | class (111 ops) | included | cch_tui | #997, #998, #999, #1000, #1001, #1002, #1006, #1010, #1011, #1017 | E13-E15 fullscreen viewport | TuiAltScreen / ViewportTUI |
| `TuiAltScreenOptions` | interface (10 ops) | cpp-representation | cch_tui | #997, #998, #999, #1000, #1001, #1002, #1006, #1010, #1011, #1017 | E13-E15 fullscreen viewport | TuiAltScreen / ViewportTUI |
| `TuiInputListener` | type | cpp-representation | cch_tui | #952, #977, #978, #980, #981, #982, #1016 | E09/E10/E11 common TUI lifecycle | TUI/Container/Component/Overlay/listeners |
| `TuiInputListenerResult` | type | cpp-representation | cch_tui | #952, #977, #978, #980, #981, #982, #1016 | E09/E10/E11 common TUI lifecycle | TUI/Container/Component/Overlay/listeners |
| `TuiMainScreen` | class (12 ops) | included | cch_tui | #983, #984, #985, #986, #987 | E12 regular renderer | TuiMainScreen |
| `TuiMainScreenRenderState` | interface (7 ops) | cpp-representation | cch_tui | #983, #984, #985, #986, #987 | E12 regular renderer | TuiMainScreen |
| `TuiMode` | type | cpp-representation | cch_tui | #952, #977, #978, #980, #981, #982, #1016 | E09/E10/E11 common TUI lifecycle | TUI/Container/Component/Overlay/listeners |
| `TuiMouseButton` | type | cpp-representation | cch_tui | #952, #977, #978, #980, #981, #982, #1016 | E09/E10/E11 common TUI lifecycle | TUI/Container/Component/Overlay/listeners |
| `TuiMouseEvent` | interface (13 ops) | cpp-representation | cch_tui | #952, #977, #978, #980, #981, #982, #1016 | E09/E10/E11 common TUI lifecycle | TUI/Container/Component/Overlay/listeners |
| `TuiMouseEventResult` | interface (4 ops) | cpp-representation | cch_tui | #952, #977, #978, #980, #981, #982, #1016 | E09/E10/E11 common TUI lifecycle | TUI/Container/Component/Overlay/listeners |
| `TuiMouseEventType` | type | cpp-representation | cch_tui | #952, #977, #978, #980, #981, #982, #1016 | E09/E10/E11 common TUI lifecycle | TUI/Container/Component/Overlay/listeners |
| `TuiStopOptions` | interface (1 ops) | cpp-representation | cch_tui | #952, #977, #978, #980, #981, #982, #1016 | E09/E10/E11 common TUI lifecycle | TUI/Container/Component/Overlay/listeners |
| `VStack` | class (2 ops) | included | cch_tui | #998, #999 | E13 VStack | VStack/StackEntry |
| `ViewportTUI` | interface (1 ops) | cpp-representation | cch_tui | #952, #977, #978, #980, #981, #982, #1016 | E09/E10/E11 common TUI lifecycle | TUI/Container/Component/Overlay/listeners |
| `WheelScrollLines` | type | cpp-representation | cch_tui + settings | #1004, #1020 | E14 wheel lines | WheelScrollLines setting type |
| `allocateImageId` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `backgroundAnsi` | function | included | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `calculateImageRows` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `colorToHex` | function | included | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `colorToOkhsl` | function | included | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `colorToOklch` | function | included | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `colorToRgb` | function | included | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `compositeTuiLine` | function | included | cch_tui | #977, #984 | E09/E12 line composition | compositeTuiLine-equivalent helper or renderer-internal with same outcomes |
| `decodeKittyPrintable` | function | included | cch_tui | #950, #951 | E02 Kitty text vs shortcut identity; legacy/modifyOtherKeys/dup | Keys parse/match/printable helpers with per-terminal state |
| `deleteAllKittyImages` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `deleteKittyImage` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `detectCapabilities` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `encodeITerm2` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `encodeKitty` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `foregroundAnsi` | function | included | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `fuzzyFilter` | function | included | cch_tui | #958 | E04 fuzzy offsets | fuzzyMatch/fuzzyFilter with original UTF-8 offsets |
| `fuzzyMatch` | function | included | cch_tui | #958 | E04 fuzzy offsets | fuzzyMatch/fuzzyFilter with original UTF-8 offsets |
| `getCapabilities` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `getCellDimensions` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `getGifDimensions` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `getImageDimensions` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `getJpegDimensions` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `getKeybindings` | function | included | cch_tui + frontend consumers | #959 | E05 binding context convergence | shared/local KeybindingsManager snapshots |
| `getNativeClipboard` | function | included | cch_tui (+ frontend_tui adapter) | #992, #993 | E18 unavailable/empty/failed Linux clipboard outcomes | injected NativeClipboard-equivalent capability interface |
| `getOsc8LinkAtColumn` | function | included | cch_tui | #956, #957 | E04 width/ANSI/slice/truncate/OSC8 | visibleWidth/wrap/slice/truncate/getOsc8LinkAtColumn/strip |
| `getPngDimensions` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `getTerminalColorMode` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `getWebpDimensions` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `hyperlink` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `imageFallback` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `indexedColor` | function | included | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `isAppleTerminalSession` | function | approved-exclusion | n/a | #946-out-of-scope | exclusion: Darwin/Apple Terminal helper | none |
| `isFocusable` | function | included | cch_tui | #977, #997 | E09/E13 capability tests | type predicates / capability queries on TUI interfaces |
| `isKeyRelease` | function | included | cch_tui | #950, #951 | E02 Kitty text vs shortcut identity; legacy/modifyOtherKeys/dup | Keys parse/match/printable helpers with per-terminal state |
| `isKeyRepeat` | function | included | cch_tui | #950, #951 | E02 Kitty text vs shortcut identity; legacy/modifyOtherKeys/dup | Keys parse/match/printable helpers with per-terminal state |
| `isKittyProtocolActive` | function | included | cch_tui | #950, #951 | E02 Kitty text vs shortcut identity; legacy/modifyOtherKeys/dup | Keys parse/match/printable helpers with per-terminal state |
| `isViewportTUI` | function | included | cch_tui | #977, #997 | E09/E13 capability tests | type predicates / capability queries on TUI interfaces |
| `matchesKey` | function | included | cch_tui | #950, #951 | E02 Kitty text vs shortcut identity; legacy/modifyOtherKeys/dup | Keys parse/match/printable helpers with per-terminal state |
| `mixColors` | function | included | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `okhslColor` | function | included | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `oklabToOkhslLightness` | const | included | cch_tui | #989 | E17 OKLCH/OKHSL/mix | oklabToOkhslLightness helper |
| `oklchColor` | function | included | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `parseColor` | function | included | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `parseKey` | function | included | cch_tui | #950, #951 | E02 Kitty text vs shortcut identity; legacy/modifyOtherKeys/dup | Keys parse/match/printable helpers with per-terminal state |
| `parseTerminalColorSchemeReport` | function | included | cch_tui + frontend_tui Theme | #990, #991 | E17 terminal color queries | parseTerminalColorSchemeReport + TerminalColors values |
| `renderImage` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `renderLatex` | function | included | cch_tui | #973, #974, #975, #976 | E08 latex | renderLatex reusable capability |
| `resetCapabilitiesCache` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `rgbColor` | function | included | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `setCapabilities` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `setCapabilityOverrides` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `setCellDimensions` | function | included | cch_tui (+ frontend converter) | #994, #995, #996, #1012, #1022 | E16 image dims/protocol/helpers | detectCapabilities/encode*/dimensions/renderImage helpers |
| `setImageTranscoder` | function | included | cch_tui + frontend_tui | #996 | E16 converter absent/fail/success | injected transcoder registration (not process-global mutable manager) |
| `setKeybindings` | function | included | cch_tui + frontend consumers | #959 | E05 binding context convergence | shared/local KeybindingsManager snapshots |
| `setKittyProtocolActive` | function | included | cch_tui | #950, #951 | E02 Kitty text vs shortcut identity; legacy/modifyOtherKeys/dup | Keys parse/match/printable helpers with per-terminal state |
| `sliceByColumn` | function | included | cch_tui | #956, #957 | E04 width/ANSI/slice/truncate/OSC8 | visibleWidth/wrap/slice/truncate/getOsc8LinkAtColumn/strip |
| `stripTerminalSequences` | function | included | cch_tui | #956, #957 | E04 width/ANSI/slice/truncate/OSC8 | visibleWidth/wrap/slice/truncate/getOsc8LinkAtColumn/strip |
| `styleText` | function | included | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `styleTextWithAnsi` | function | included | cch_tui | #988, #989 | E17 color parse/style | Color values, parseColor, styleText*, mixColors |
| `truncateToWidth` | function | included | cch_tui | #956, #957 | E04 width/ANSI/slice/truncate/OSC8 | visibleWidth/wrap/slice/truncate/getOsc8LinkAtColumn/strip |
| `visibleWidth` | function | included | cch_tui | #956, #957 | E04 width/ANSI/slice/truncate/OSC8 | visibleWidth/wrap/slice/truncate/getOsc8LinkAtColumn/strip |
| `wrapTextWithAnsi` | function | included | cch_tui | #956, #957 | E04 width/ANSI/slice/truncate/OSC8 | visibleWidth/wrap/slice/truncate/getOsc8LinkAtColumn/strip |

## Diagnostics and non-index helpers

| Id | Classification | Tickets | Notes |
| --- | --- | --- | --- |
| `diag.PI_TUI_WRITE_LOG` | included | #1016 | env-gated in pi; Linux-usable diagnostic retained |
| `diag.debugKey` | included | #1016 |  |
| `internal.alt-screen-search` | cpp-representation | #1010 | not a package index export; accounted because public search behavior depends on it |
| `internal.layout` | cpp-representation | #998, #999, #1000, #1002 | not a package index export |
| `internal.kill-ring` | cpp-representation | #961 | not a package index export |
| `internal.undo-stack` | cpp-representation | #961 | not a package index export |
| `internal.native-modifiers` | approved-exclusion | #946-out-of-scope | platform-exclusive synthesis excluded |

## Consumer migration notes

- Accounting only: this document does not implement behavioral parity.
- Actual named evidence consumers remain the #947 differential/golden readers; later tickets extend scenarios using this ledger's ticket assignments.
- Historical gap inventories remain provenance for older baselines; they are not authority for `pi-v1.0.4` inclusion.
- Missing coverage discovered later must become an explicit small implementation ticket before downstream acceptance.

