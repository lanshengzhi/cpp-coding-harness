# pi-tui compatibility fixtures

## Baseline routing

The inventory and gate report below describe historical evidence at `83114817c68f5413e4d7ba6d7003ddc511cd31d2`. [ADR 0067](../../docs/adr/0067-align-the-linux-tui-toolkit-and-native-tui-with-pi-v1-0-4.md) and [#946](https://github.com/lanshengzhi/cpp-coding-harness/issues/946) select the new Named Baseline `pi-v1.0.4`, exact revision `7c10bd4337495ee613f2224843ecdf349b80d1df`, for complete Linux TUI alignment. Historical Deferred classifications and behavioral exemptions do not exempt included capabilities from that target.

[#947](https://github.com/lanshengzhi/cpp-coding-harness/issues/947) introduces the separate named capture/replay runner; [#948](https://github.com/lanshengzhi/cpp-coding-harness/issues/948) accounts for capabilities and actual consumers in [`docs/research/tui-v1.0.4-capability-ledger.md`](../../docs/research/tui-v1.0.4-capability-ledger.md) / [`.json`](../../docs/research/tui-v1.0.4-capability-ledger.json). Keep existing fixture provenance. A missing, wrong-revision or tampered requested bundle must fail rather than fall back to these fixtures. The named bundle below records only the initial runnable evidence slice; it does not accept the remaining toolkit or product capability target.

## Named pi-v1.0.4 evidence runner

[`baselines.json`](baselines.json) selects the exact frozen revision and pins the digest of
[`bundles/pi-v1.0.4/manifest.json`](bundles/pi-v1.0.4/manifest.json). The manifest binds every
artifact to its SHA-256 and byte count. Each artifact also carries its baseline, revision,
source endpoints, timestamp, environment, scenario dimensions, inputs and observed outputs.
The initial independently captured cases are:

| Family | Frozen observation | Real C++ consumer |
| --- | --- | --- |
| `input.json` | `parseKey` press/repeat/release identities; actual Input editing values and submit callbacks (`a`, `b`, Left, `c`, Enter → `acb`) | `PiTuiDifferentialTest` drives the decoder and real focused Input through Tui/VirtualTerminal |
| `component.json` | Text wrapping, padding and full ANSI output before/after `setText` | `PiTuiDifferentialTest` drives public Text render/mutation |
| `screen-state.json` | Regular renderer frames and common-root add/remove/focus/input/clear/stop; every cell's grapheme, continuation and style, visible rows, scrollback, cursor position/visibility and callback order | `ScreenStateGoldenTest` drives Component/Focusable/MainScreen through `ITui` over VirtualTerminal |
| `capability-ledger.json` | Frozen `packages/tui/src/index.ts` export set (173) with ledger classifications and required cross-references | `TuiCapabilityLedgerTest` + docs ledger; offline verify requires the family |
| `fuzzy.json` | `fuzzyMatch` scores/matches and `fuzzyFilter` rankings over non-ASCII, combining, supplementary, final-sigma and no-break-space queries and texts, each row recording both UTF-16 and UTF-8 lengths of the text | `PiTuiDifferentialTest` drives `fuzzy_match`/`fuzzy_filter`; `FuzzyTest` pins the retained original-UTF-8 offset helper that pi does not export |
| `latex.json` | Frozen `renderLatex` inline grammar (symbols, nested groups, fractions including `frac 1/2`, roots, accents, inline scripts, operators), its `undefined` failure value, and the `display` option | `LatexTest` replays every row through `cch_tui::render_latex` and compares complete values |
| `layout.json` | Constrained-frame allocation and painting: `VStack` grow/shrink/min/max/gap allocation, an auto-basis dock measured through nested minimum sizes, a leaf cropped upward to keep its `CURSOR_MARKER` row, a responsive `visible` entry under two cross-axis alignments, the transcript/editor/menu/footer dock driven through growth, menu appearance, over-constraint and shrink, and the horizontal layout: `HStack` align/grow/shrink/clamp allocation composed into shared rows, unequal-column frames, an auto-basis entry measured at the frame width, a nested `VStack`-in-`HStack` frame re-framed through inner growth, removal and re-add, and an `HStack` grown inside a constrained `VStack` | `StackLayoutEvidenceTest` and `HStackEvidenceTest` replay every frame through `cch_tui::detail::LayoutEngine` and compare child offsets, heights, `lineOffset`, painted rows and cursor |
| `scroll-view.json` | Scroll algebra and composition: follow-end transitions with the unused delta of every `scrollBy`, appended rows moving a followed window, an absolute `scrollTo` with `disableFollow` at the last row that keeps its position while rows are appended, nested viewport measurement with the `getScrollViewsAt` depth order, placement crop rows and hidden source rows, and the implicit alternate-screen viewport across scroll, append, jump and resize | `ScrollViewEvidenceTest` replays every scenario through the public `cch_tui::ScrollView`, `LayoutEngine` and `AltScreen` seams over `VirtualTerminal` and compares scroll position, follow state, viewport and content heights, painted rows and image placements |
| `alt-screen-image.json` | Fullscreen kitty placement lifecycle: a scrolled image cropped to its visible source band and restored, an anchor moved by a growing sibling re-emitted as a placement-only `a=p` command, image removal and session teardown deletions, and both offscreen eviction caps deleting by image id; every phase records the image commands with the screen row each executed at plus the buffered screen | `AltScreenImageEvidenceTest` replays every phase through `AltScreen` over `VirtualTerminal`, renames the random kitty ids to first-appearance indices and compares the command trace and the viewport |
| `iterm2-image.json` | A fullscreen iTerm2 session's real `Image` component: the fallback row and no image sequence before or after `stop` | `AltScreenImageEvidenceTest` replays it through `AltScreen` over an iTerm2 `VirtualTerminal` |

`latex.json` was appended to the existing bundle rather than re-captured with all of it, so it keeps the envelope `capturedAt` and records its own wall clock plus the pinned published build digest under `recapture`. The other five families are unchanged.

The screen case changes bold red `red` to underlined green `go` and retains a focused cursor
at column 2, row 1. A text-only screenshot would miss a wrong style or displaced cursor;
the replay compares all of them, including erased and empty cells. The frozen test-local
cursor marker becomes equivalent out-of-band C++ cursor metadata. Palette colors use
canonical SGR representations. The cell comparison maps xterm's vacant-cell empty string
and C++'s explicitly padded U+0020 to the same visible blank, retaining every continuation
bit and style. Viewport strings use xterm's trim-right representation, while complete cell
comparisons retain all trailing cells and their attributes. Original captured bytes remain
unchanged; no text, nonblank glyph, style, cursor or geometry is projected away. This initial capture uses no images, OSC 8 links, scrolling,
real emulator or IME; those require their own later-ticket evidence.

For #977, the named runner re-captured all 11 current families at the same frozen revision
and added `common-root-lifecycle` to `screen-state.json`. Its replay exercises `ITui` with
real component ownership and compares the initial, removed, cleared and stopped screens,
all cells and cursor state, plus focus/input callback order. Unlike an interface-existence
check, it detects detached content remaining visible, input going to the wrong component,
and the deferred-wrap cursor left by clearing a full-width row. Separate regressions cover
reentrant removal lifetimes, scrolled dock teardown, and concurrent terminals with different
image protocols and late cell-size replies without modifying global capability state.

For #998, the named runner re-captured all 12 current families at the same frozen revision and
added `layout.json`. Its seven scenarios are read through pi's own `VStack`, `renderLayoutFrame`,
`stripTerminalSequences` and bottom-up cursor extraction, so a silently different allocation,
composition order, gap, crop or cursor translation fails the replay: the grow case distinguishes a
fixed basis from a flexible one, the shrink case distinguishes proportional give-back from an equal
split, the nested case distinguishes a measured auto basis from a fixed one, the cursor case
distinguishes an upward crop from a top crop, and the dock frames distinguish a measured dock from
an unmeasured one while requiring both footer rows in every frame (the live property of #831).
`capture/stack-layout-scenarios.mts` executes the frozen component directly; its source digest is
recorded alongside the main generator digest.

For #999, the same runner re-captured every family and added six horizontal cases to `layout.json`:
the standalone `HStack` align, grow, clamp and zero-only runs, the unequal-column frame at two
widths, the four frame alignments, an auto-basis entry, a nested `VStack`-in-`HStack` frame re-framed
after an inner line growth, a child removal and an auto-basis re-add, and an `HStack` grown inside a
constrained `VStack`. Every case records observations a plausible implementation gets wrong in one
direction only: the align runs separate the cross-axis offset from the column allocation and record
the width each entry was rendered at, so a numeric basis that is measured anyway, an entry that is
rendered at its allocated width a second time, or a zero-width entry that is rendered at all fails;
the unequal frames pin the sequential take-back, minimum saturation and the composed row
and the recursive box tree, so an overwritten row fails where a spliced one passes; the auto-basis
case separates a measurement at the frame width from a natural-width one; the nested frame separates
an allocation-driven crop from a slice and a live mutation from a rebuild; and the in-`VStack` case
separates an `HStack` allocating the width it was given from one allocating the root width.
`capture/stack-layout-scenarios.mts` executes the frozen components directly; its digest is recorded
in the manifest.

For #1000, the named runner re-captured all 13 current families at the same frozen revision and
added `scroll-view.json`. Its six scenarios are read through pi's own `ScrollView`,
`renderLayoutFrame`, `getScrollViewsAt`, `cropKittyImageLine` and a real `TuiAltScreen` over pi's
`VirtualTerminal`, so a silently different scroll position, follow state, unused delta, viewport
order or cropped row fails the replay: `follow-transitions` distinguishes a followed end from a
scrolled-away one and pins the unconsumed lines of each step, `follow-append` distinguishes a window
that moves with appended rows from one that keeps its position, `scroll-to-disable-follow`
distinguishes an absolute `scrollTo` at the last row with `disableFollow` from the same target
without it (the window stays while rows are appended, and a plain scroll to the end follows again),
`nested-scroll` distinguishes the inner viewport's own measurement and delta ownership from the
enclosing one's, `scroll-image-crop` pins the destination rows and hidden source rows of a placement
cut by a stack row and by a scrolled window (#1012 owns the protocol crop), and
`alt-screen-implicit-follow` distinguishes the implicit follow-end viewport a rootless child is
mounted behind across scroll, append, `scrollToBottom`, `scrollToTop` and resize.
`capture/scroll-view-scenarios.mts` executes the frozen components directly; its source digest is
recorded alongside the main generator digest.

For #1001, the named runner re-captured all 15 current families at the same frozen revision and
added `scrollbar.json`. Its five scenarios are read through pi's own `ScrollView`,
`renderLayoutFrame` and a real `TuiAltScreen` over `VirtualTerminal`, so a silently different
reservation, transient lifetime, thumb proportion or hover fails the replay:
`scrollbar-modes-and-reservation` distinguishes the three modes and pins the reserved content
column, the child width and the re-wrapped frame of a runtime mode change;
`scrollbar-transient-timeline` distinguishes a hidden first frame from the revealed, hovered,
timer-expired, scrolled-to-end and scrolled-to-start frames, and pins the track/thumb colour rows,
the preserved-background re-emit, the followed window and the fitting cases;
`scrollbar-geometry-edges` pins the 19/10/4/2 thumb proportions of a twenty-row track, the
empty-content `Infinity` branch, a one-row track and a single-column viewport;
`scrollbar-overlay-preserves-background` distinguishes the background an overlay glyph keeps from
the border foreground it drops; and `scrollbar-alt-screen-hover` distinguishes a wheel over the
track, a motion that keeps the hover, a motion that leaves it to the hide timer, a focus loss and
`stop()` from each other. `capture/scrollbar-scenarios.mts` executes the frozen components
directly; its source digest is recorded alongside the main generator digest.

For #1004, the named runner re-captured all 16 current families at the same frozen revision and
extended `scroll-view.json` with `wheel-accelerator`, `nested-wheel-chaining` and
`alt-screen-navigation-indicator`. They execute the frozen `WheelScrollAccelerator` and
`TuiAltScreen` over pi's `VirtualTerminal`: fixed and automatic wheel line counts, velocity
scaling, same-notch burst handling, direction/pause resets and fractional carry are pinned by
individual streams; the nested route proves the inner viewport consumes two of five lines, its
parent consumes the remaining three, and a side-by-side viewport stays fixed; and the fullscreen
case distinguishes focused-editor Home/End input from Ctrl+Home/End viewport navigation, checks
key releases leave the viewport alone, and checks a wheel-up indicator click returns to the end.
The C++ replay drives `AltScreen` through `VirtualTerminal` and checks both frozen outputs and the
cases that distinguish consumption, chaining, key routing and click handling. The capture asserts
the nested and fullscreen observations while running against frozen pi, and its source endpoints
name the accelerator, keybindings and indicator handlers. `capture/scroll-view-scenarios.mts`
executes these cases directly; its source digest is recorded in the manifest.

For #1006, the named runner re-captured all 16 current families at the same frozen revision and
added `selection.json`. Its four scenarios execute pi's `TuiAltScreen`, `Text` and
`VirtualTerminal`, and record the visible selection cells, exact copied text, copy-on-select
invocations and lifecycle: `styled-cjk-wrapped-selection` selects across a wrapped styled row and
a wide CJK grapheme; `selection-content-focus-and-clear-lifecycle` checks same-shape content
updates, focus out/in and ordinary content clicks; `selection-copy-specific-failure` pins the
injected failure outcome and its visible text; and `copy-on-select-default-and-empty-range` pins
the default true setting and proves an empty click invokes no copy. The replay consumes this
verified artifact directly; expected values are not generated from Pike output.
`capture/selection-scenarios.mts` executes the frozen components directly, and its digest is
recorded in the manifest.

For #1009, the same artifact adds `osc8-click-and-drag-routing` and `linux-secondary-button-paste`.
The OSC 8 case records the frozen character click opening on release, a count-two
word-granularity click that does not open another URL, and a drag that does not open a URL. The
frozen OSC 8 capture click is replayed against Pike's OSC 8 `Text` to verify link routing. The Linux
case records frozen pi's no-op for the secondary button. Issue #1009 explicitly requires the Linux
route, so Pike handles raw unmodified button 2 after component dispatch declines and before text
selection, invokes the native clipboard path once on press, and ignores release and motion reports.
The frozen Windows-only gate is
`packages/tui/src/tui-alt-screen.ts` `handleRightClickPaste` (lines 1011–1028); its named test is
`packages/tui/test/tui-alt-screen.test.ts` `invokes the right-click paste handler only on Windows outside VS Code`
(lines 421–457). This Linux behavior is an intentional compatibility deviation. The scenarios are
captured from the unmodified frozen checkout.

For #1007, the same artifact holds eight frozen multi-click scenarios for pi's word/line selection
cycle: `multiclick-word-line-cycle` (press/double/triple over one word), `multiclick-word-joiners-and-classes`
(`/` `-` joiner chains, the rule-based word classes and dictionary-script boundaries),
`multiclick-word-drag-expansion` and `multiclick-line-wrapped-rows` (granularity-preserving
drags), `multiclick-expiry-and-word-identity` (the inclusive 500 ms interval and the word-identity
reset; the exact 500 ms/501 ms boundary itself is pinned by the deterministic-clock unit test,
since this runner's wall-clock sleeps cannot land exactly on the inclusive boundary), `multiclick-focus-out-cancels`
and `multiclick-scrollbar-cancels` (the stale-click cancellation sites), plus
`multiclick-dictionary-script-runs` (double-clicks inside multi-character Han `日本語`,
Hiragana `こんにちは` and Thai `สวัสดีครับ` runs select whole ICU word segments). These eight
#1007 scenarios join four #1006 scenarios and two #1009 scenarios for 14 total in `selection.json`.
Word segments come from pi's `Intl.Segmenter` (ICU word-break
dictionaries and rules); Pike replays them through the pinned vcpkg ICU breaker
(`cch::tui::detail::segment_words`, UBRK_WORD), the same engine class. Correction to an earlier
packet claim: frozen pi does NOT clear its click state on content resize or content change — it
clears only at session start, focus out, drag motion and scrollbar press (the word-range identity
check is the staleness guard), and the C++ mirrors that; criterion 3's "content resize cancels"
wording must not be attributed to frozen behavior.

For #1002, the named runner re-captured all 14 current families at the same frozen revision and
added `mouse.json`. Its five scenarios are read through pi's own `MouseRegion`, `Container`, `Box`,
`TuiAltScreen` and `dispatchMouseEvent`, so a silently different hit path, coordinate transform,
capture lifetime, click count or wheel ownership fails the replay: `cached-frame-hit-test`
distinguishes a press routed to the deepest nested region from one that stops at the padding box,
and pins every candidate box's rect, the dispatch order, the resolved target, its local
coordinates, its size, its focus target and its render request together with the render counts
before and after the probes; `capture-drag-and-host-focus` distinguishes a captured drag and
release outside the component's bounds from one that stops at them, and a delegating host that
keeps focus from a plain region that takes it; `wheel-consumed-and-unconsumed` distinguishes a
consumed wheel report that leaves the viewport from an unconsumed one that moves it and back;
`motion-and-click-counts` distinguishes handled motion that writes no frame from one that
repaints, and the wrapping click counter from a constant one; and
`regular-screen-keeps-terminal-mouse` distinguishes a main screen that enables no mouse reporting
and turns no packet into text from one that does. `capture/mouse-scenarios.mts` executes the
frozen components directly; its source digest is recorded alongside the main generator digest.

For #1003, the named runner appended one family and re-captured nothing else: `mouse-components.json`
records 12 mouse editing and list/settings selection scenarios through the frozen `Input`, `Editor`,
`SelectList`, `SettingsList`, `Container` and `TuiAltScreen`, so the toolkit's `handle_mouse`
arithmetic is measured against the frozen components rather than a re-derived value.
`capture/mouse-components-scenarios.mts` holds the scenario bodies and is emitted by the main
generator; `capture/recapture-mouse-components.mts` is the append recipe (same frozen revision,
same `capturedAt`, every other artifact byte-identical). The input scenarios pin the press-to-cursor
column for a value column, a scrolled window's start column and a wide-grapheme run, and record that
move, drag, release and wheel stay unhandled; the select-list and settings-list scenarios pin press
select, click activation through the renderer gesture, the wheel's centered visible window, hover
never selecting, the search row forwarding to its input (with the input's cursor column) while the
separator row stays inert, and a submenu click selecting and closing while its host keeps focus; the
delegating-host and overlay scenarios pin that a nested click keeps the host focused, including the
frozen `dispatchMouseToOverlay` walk over the rendered overlay layouts with overlay-local
retargeting; the editor scenarios pin the click's bounded row mapping across the border, the content
rows, wrapped segments and wide graphemes, and a drag that copies instead of moving the cursor.
Observations are read after `handleMouse` returns, so a post-click cursor or row is the frozen
component's own state. Two recorded divergences stay outside the comparison: frozen hardcodes the
settings hint text while the toolkit spells the same line from the keybinding registry, and the
editor's border rows need the C++ border hook to exist at all.

For #1017, the named runner appended one family and re-captured nothing else: `cli-tui-mode.json`
records seven CLI TUI-mode scenarios through the frozen `packages/coding-agent/src/cli/args.ts`
(`parseArgs`, `printHelp`) and `core/settings-manager.ts` (`fromStorage`, `getTuiMode`,
`setTuiMode` over a storage double), so the resolved mode surface is measured against the frozen
module rather than a re-derived value. Each flag scenario keeps every field the `--tui-mode` branch
can move — the mode, the positional messages, the print flag, the diagnostics and the unknown-flag
bucket — and `tui-mode-flag-rejects-a-missing-value-without-consuming` distinguishes a missing value
(diagnosed, nothing consumed, so `--print` still engages and `hello` still becomes a message) from a
repeated token that yields *one* diagnostic *and* a resolved mode, while
`tui-mode-flag-rejects-an-unknown-mode-after-consuming-it` distinguishes a consumed unknown value
(the next token is still a message) from an accepted value that a later rejection leaves in place.
`settings-tui-mode-read-normalization` records that the frozen `getTuiMode` compares against
`"regular"` only, so an out-of-union stored value still resolves `fullscreen`, and
`settings-tui-mode-write-persists-the-global-key` records `setTuiMode` rewriting the global document
with the key added and the unrelated keys preserved.
`tui-mode-flag-over-settings-precedence` records the expression
`options.tuiMode ?? this.settingsManager.getTuiMode()`
(`packages/coding-agent/src/modes/interactive/interactive-mode.ts:604`) applied to five observed
operand pairs — each with the stored document it was read from, one of them unset and one a rejected
flag — and states that the expression itself needs the full interactive runtime, so the capture
executes neither it nor `runInteractiveMode`; only the operands are observations. Two divergences are
measured rather than assumed: frozen resolves an unset `tuiMode` to `fullscreen` while Pike answers
`regular` until #1024 flips the default, and frozen leaves the inline spelling `--tui-mode=regular`
in `unknownFlags` (mode unset, no mode diagnostic, and its session services later report the
unclaimed flag) while Pike's parser gives every value option a `--name=value` form and therefore
resolves that spelling to `regular`. `capture/cli-tui-mode-scenarios.mts` holds the scenario bodies
and is emitted by the main generator; `capture/recapture-cli-tui-mode.mts` is the append recipe (same
frozen revision, same `capturedAt`, every other artifact byte-identical). `CliTuiModeEvidenceTest`
replays the family through the Pike parser, the settings manager and `select_tui_mode`, comparing a
frozen diagnostic as the failed parse that carries the frozen message text.

For #1036, the named runner appended one more family and re-captured nothing else:
`chat-viewport.json` records ten scenarios of the product two-region fullscreen viewport through the
frozen `createChatViewport`
(`packages/coding-agent/src/modes/interactive/chat-viewport.ts:22`), composed from plain
`Container`/`Text` regions and driven through the frozen `TuiAltScreen` and the frozen
`VirtualTerminal`: a short document whose free rows return to the dock, an overflowing document
pinned to its end, an append that keeps following, a narrow terminal whose long editor line wraps
inside the dock while the transcript keeps its own rows, a resize smaller and larger with both
footer rows surviving, a one-line editor floored at the dock's `minSize: 3` beside a seven-row
editor that takes the rows the document yields, a wheel scroll that moves the transcript window
while every dock row stays, the frozen arbiter pair (the scrollbar defaults to `auto` and an
explicit `hidden` override is honoured, with the automatic bar's styled cells recorded), and the
optional widget regions keeping their frozen dock order (pending, status, widgets above, editor,
widgets below, footer). Every step records the complete frame rows, cursor position, cursor
visibility and the transcript's `scrollTop`/`viewportHeight`/`isFollowingEnd`/`scrollbar`/
`isScrollbarVisible`, and a step whose frame draws any styled cell fails the capture unless it also
records the complete styled grid — so a style cannot hide in an unrecorded frame and the replay
compares every attribute of every frame that has one. The product call site
(`interactive-mode.ts:961-980`, `:868-875`) and the `setLayoutRoot`-over-mounted-children alias need
the full interactive runtime, so they are recorded as cited source endpoints and are not executed
here. `capture/chat-viewport-scenarios.mts` holds the scenario bodies and is emitted by the main
generator; `capture/recapture-chat-viewport.mts` is the append recipe (same frozen revision, same
`capturedAt`, every other artifact byte-identical). `ChatViewportEvidenceTest` replays the family
through Pike's `create_chat_viewport`, `AltScreen::set_layout_root` and `ScrollView`. Two
divergences are recorded rather than assumed. Frozen's call site passes both `scrollbarTrackStyle` and
`scrollbarThumbStyle` while Pike's live theme carries no `scrollbarTrack` token yet, so the product
assembly supplies only the thumb hook and the track keeps the toolkit's pi dim-gray default. And frozen
passes `widgetContainerAbove`/`widgetContainerBelow` into the dock (fed from `extensionWidgetsAbove` /
`extensionWidgetsBelow`, `interactive-mode.ts:2483-2484`), while this product has no widget containers at
all, so the assembly passes no widget entry; `create_chat_viewport` itself accepts both optional entries
and the captured scenarios record the frozen dock order they take.

For #1018, the named runner appended one more family and re-captured nothing else:
`renderer-switch.json` records eight scenarios of the live `switchTuiMode` renderer swap
(`packages/coding-agent/src/modes/interactive/interactive-mode.ts:883`, driven through the frozen
arbiter's `Object.create(InteractiveMode.prototype)` context trick) and of `stopInteractiveTui`
(`:874`) through the frozen `TuiMainScreen`, `TuiAltScreen` and `VirtualTerminal`: the same-mode
call that returns `true` without touching the renderer even while a visible overlay entry exists
(the no-op check runs before the overlay guard); the overlay-entry refusal in two cases, hidden
and visible, each leaving the mode, focus, child list and terminal counters untouched and writing
zero bytes; regular to fullscreen and back with the same children, focus and editor text and the
frozen rebind order (`stop` the retired root, `start` the replacement, then theme rebind, extension
unsubscribe and listener rebind); the main-screen render state crossing out and back (the switch
captures the live 28-column, 11-line main screen and the returning root restores exactly those
lines); `startRenderer = false` completing the swap without starting the replacement root; the
progress-restore condition over five operand cases; and the transcript exit, which drains the
overlay stack, switches to regular without starting it, renders once and stops the terminal twice.
Every step records the complete frame rows, cursor position, cursor visibility and the transcript's
`scrollTop`/`viewportHeight`/`isFollowingEnd`/`isScrollbarVisible`, plus the switch's own trace: the
callback order, the invalidated regions, the bytes the switch wrote and the terminal's start/stop
counts. Exactly one frame per direction is designated styled (the fullscreen frame of the
regular-to-fullscreen scenario and the regular frame of the fullscreen-to-regular one); those two
record the complete cell grid with every style attribute and the scrollback, every other frame
fails the capture if it draws any styled cell, and the styled frames replay a document whose last
row carries the frozen `\x1b[31m` wrap, so a dropped or misplaced style fails the replay.
`capture/renderer-switch-scenarios.mts` holds the scenario bodies (including the runtime-only Azure
provider stub the frozen module graph needs) and is emitted by the main generator;
`capture/recapture-renderer-switch.mts` is the append recipe (same frozen revision, same
`capturedAt`, same `environment`, every other artifact byte-identical, manifest 23 → 24 with the
record's `sourceEndpoints` and source digests). `RendererSwitchEvidenceTest` replays six recorded
frames — both styled grids through fresh Pike `MainScreen`, `AltScreen`, `set_layout_root` and
`create_chat_viewport` instances — and checks the recorded matrix. Those frame replays do not
execute Pike's live switch. A separate live `InteractiveEngine` case compares the recorded target
mode, result and draft text, preserves focus identity across a round trip, and verifies refusal
with a responsively hidden overlay entry. `TuiModeSwitchTest` also measures one extension input
delivery per input and no repeated registration or editor factory invocation across both directions.
Theme query bytes and the complete live switch protocol trace are not compared yet. Four gaps or
adaptations are recorded rather than assumed: frozen
`switchTuiMode` reads `previousUi.getShowHardwareCursor()` (`:891`) and hands it to the replacement
renderer while Pike has no hardware-cursor read-back and never transfers one, so each selected root
keeps its own default; frozen reads `previousUi.onDebug` (`:893`) while Pike reapplies the debug
hook it owns (`InteractiveEngine::apply_debug_hook`); frozen restores progress from
`restoreProgress && getShowTerminalProgress() && (isStreaming || isCompacting)` (`:926-931`) while
Pike has no terminal-progress setting and no `Terminal::set_progress` caller, so the capture records
that condition as an expression over the frozen rows instead of a driven behavior; and frozen's
private seam takes `restoreProgress` and `startRenderer`, which Pike's
`InteractiveEngine::switch_tui_mode(TuiMode)` does not have, so the `startRenderer = false` scenario
is recorded rather than re-driven. `switchTuiMode` and `stopInteractiveTui` themselves are private
and need the full interactive runtime, so they are cited source endpoints too.

For #1037, the named runner appended one more family and re-captured nothing else:
`hardware-cursor.json` records thirteen scenarios of hardware cursor visibility, preference
controls and IME positioning through frozen `TuiMainScreen`, `TuiAltScreen` and `VirtualTerminal`
with deterministic components: default preference (`false`), enabled (`true`), explicit disabled
(`false`), dynamic preference toggle (`false` -> `true` -> `false`), invalid / null cursor
(hidden regardless of preference), nested components (Container + Box padding), and clipped /
scrolled coordinates (off-screen cursor hides). It captures terminal events (write, hideCursor,
showCursor), exact cursor rows and columns, cursor visibility, and raw writes across both roots.
`capture/hardware-cursor-scenarios.mts` holds the scenario bodies; `capture/recapture-hardware-cursor.mts`
is the append recipe (same frozen revision, same `capturedAt`, same `environment`, every other artifact
byte-identical, manifest 24 → 25 with the record's `sourceEndpoints` and source digests).
`HardwareCursorEvidenceTest` replays the family through Pike's `MainScreen`, `AltScreen`, and
`VirtualTerminal` over `ITui`. Replay asserts exact terminal cursor coordinates (`cursor.col`, `cursor.row`),
visibility mode (`cursorVisible`), preference state (`showHardwareCursor`), movement escape byte sequences
(`terminalEvents[].bytes`), and the protocol ordering of movement versus visibility operations (`terminalEvents`). Idempotent
redundant visibility-call suppression in AltScreen and incidental frame paint padding in MainScreen
invalid-cursor are bounded and verified at the public terminal decorator boundary.

For the product-level hardware cursor and live mode switch contract (#1037),
`product-cursor.json` records seven scenarios covering `SettingsManager.prototype.getShowHardwareCursor`
resolution (8 cases spanning global/project scopes, project trust, and `PI_HARDWARE_CURSOR` env fallback)
and live preference preservation across `InteractiveMode.prototype.switchTuiMode` with multiline
cursor positioning, nested Box padding, and null/undefined cursor hiding.
`capture/product-cursor-scenarios.mts` defines the scenarios; `capture/recapture-product-cursor.mts`
is the append recipe (same frozen revision `7c10bd4337495ee613f2224843ecdf349b80d1df`, same `capturedAt`,
same `environment`, every other artifact byte-identical, manifest 25 → 26 with `sourceEndpoints` and source digests).
`ProductCursorEvidenceTest` replays settings resolution against `SettingsManager` and live switch scenarios
across `MainScreen` and `AltScreenHost`, verifying exact terminal coordinates, visibility mode, preference
transfer, and frame buffer contents against the verified frozen artifact.

`product-cursor-layout.json` records product cursor layout and protocol across mode switches (#1037, spec #946)
using actual `createChatViewport` and `InteractiveMode.prototype.switchTuiMode` with controlled 4-line document,
0-line pending, 2-line status, 3-line editor, and 2-line footer regions on a 48x24 terminal across all 4
switch directions and preference configurations. In Fullscreen mode, the dock allocation pins status (2),
editor (3), and footer (2) to the bottom 7 rows, placing the cursor at row 20 invariant of transcript height.
In Regular mode, sequential flow places the prompt line at row 7 (4 document + 0 pending + 2 status + 1 cursor line).
Two real layout clipping scenarios record valid in-bounds cursor placement (line 1 of 3 editor lines) under
constrained terminal height (48x2) where editor line 1 is clipped from the visible window (producing `cursorVisible: false`)
in both directions, followed by dynamic restoration to `(5, 7)` in Regular and `(5, 20)` in Fullscreen upon resizing
to 48x24. Protocol frame boundaries are established explicitly: in AltScreen (`TuiAltScreen`), the target frame is
enclosed within `begin_synchronized_update` and `end_synchronized_update` with paint lines emitted via row-addressed
`\x1b[<row>;1H\x1b[2K` writes and trailing IME placement/visibility bundled prior to `\x1b[?2026l`; in MainScreen
(`TuiMainScreen`), the target frame paint buffer is emitted within synchronized output followed by standalone relative
movement writes (`\x1b[...A/B\x1b[...G`) and terminal visibility events (`showCursor`/`hideCursor`). Decoders retain
exact constituent cursor commands and visibility in original order without synthesis or move suppression.
`capture/product-cursor-layout-scenarios.mts` defines the scenarios; `capture/recapture-product-cursor-layout.mts`
is the append recipe (same frozen revision, same `capturedAt`, same `environment`, manifest 26 → 27).

`product-progress.json` records product terminal progress settings resolution, surgical nested persistence,
mode switch restoration, and protocol constants (#1037, spec #946) covering `terminal.showTerminalProgress`
resolution across scopes and project trust, surgical nested persistence in global settings, mode switch progress
restoration across streaming/compaction states, and exact escape sequence definitions (`\x1b]9;4;3\x07` active,
`\x1b]9;4;0\x07` clear).
`capture/product-progress-scenarios.mts` defines the scenarios; `capture/recapture-product-progress.mts`
is the append recipe (same frozen revision `7c10bd4337495ee613f2224843ecdf349b80d1df`, same `capturedAt`,
same `environment`, every other artifact byte-identical, manifest 27 → 28).
`ProductProgressEvidenceTest` replays settings resolution, surgical persistence, switch restoration expectations,
and protocol sequences against `SettingsManager` and `InteractiveEngine` from the verified frozen artifact.

`product-progress-protocol.json` records authoritative independent product terminal progress protocol
execution (#1037, spec #946) using real frozen `ProcessTerminal` stdout capture and `InteractiveMode`
seams: raw OSC 9;4 sequences (`\x1b]9;4;3\x07` active, `\x1b]9;4;0\x07` clear with no trailing semicolon)
and 1-second keepalive interval lifecycle, real `InteractiveMode.prototype.handleEvent` driving
stdout across `turn_start`, `agent_end`, `compaction_start`, and `compaction_end` (with unrelated UI
stubs explicitly documented), full 16-case truth matrix and restoration suppression in `switchTuiMode`
retaining all raw ordered writes, direct `SettingsManager.setShowTerminalProgress(false)` setting toggle
measurement proving stale progress retention until shutdown teardown, and `InteractiveMode.prototype.stop`
clear sequence counts across active/idle/toggled states. Historical artifact 28 (`product-progress.json`,
which exercised a manual `VirtualTerminal` double and literal constants) is partial and remains preserved
byte-for-byte; its test acceptance role is demoted to supplemental historical checks while appended 29th
artifact `product-progress-protocol.json` establishes the authoritative protocol acceptance boundary without
deleting existing bytes.
Runtime observation boundaries are recorded honestly: the raw keepalive phase observes presence/absence
of the background interval, not its 1000ms wall-clock timing (`expected.keepaliveIntervalMs` is a source-derived
literal); switch capture observes the synchronous method boundary and suppresses cleanup/queued paints after
`stopCapturing` (the brief observer sleep is cleanup, not proof all async output is retained); the accepted
focus is synchronous OSC 9;4 transitions rather than complete paint/query parity; scenario 5 retains independent
exact clear counts across active/idle/toggled states, but not raw writes (scenario 4 phase 4 and scenario 1
`stopActive` retain raw stop protocol); and the both-streaming-and-compacting matrix state is a synthetic
truth-predicate context rather than proof the live session reaches both simultaneously.
Public C++ acceptance tests in `ProductProgressEvidenceTest.cpp` and `TuiModeSwitchTest.cpp` consume
`product-progress-protocol.json` directly through real `InteractiveEngine`, `AgentSession`, and `SettingsManager`
without stand-in counts, asserting ordered exact OSC 9;4 byte sequences across covered streaming, idle, and
compaction lifecycle states in both switch directions (`fullscreen -> regular` and `regular -> fullscreen`),
measuring compaction start synchronized on `CompactionStartEvent` delivery and compaction end upon `compact()`
completion, verifying settings selector UI navigation, persistence, independent `SettingsManager` re-read,
separate live `/reload` preference continuity, and verifying exact shutdown teardown clear counts.
The protocol parser retains malformed sequences verbatim so exact vector comparison distinguishes delimiter
deviations from canonical forms. The test suite also validates the composed boundary where an active streaming
turn is toggled disabled in the settings UI: switching modes physically stops the previous root (emitting the
independently observed terminal stop clear) while the target root suppresses restore, with preference continuity
holding across both switch directions and through subsequent disabled turn and compaction lifecycles.
`capture/product-progress-protocol-scenarios.mts` defines the scenarios;
`capture/recapture-product-progress-protocol.mts` is the append recipe (same frozen revision
`7c10bd4337495ee613f2224843ecdf349b80d1df`, same `capturedAt`, same `environment`, every other artifact
byte-identical, manifest 28 → 29).

`product-theme-rebind.json` records supplemental partial observations for product theme rebinding (#1037, spec #946).

`product-theme-rebind-protocol.json` records authoritative independent product terminal theme rebinding protocol
vectors (#1037, spec #946) using real frozen `ProcessTerminal`/`VirtualTerminal` capture and `InteractiveMode`
controlled-context seams: exported ordered vectors for startup, scheme queries, and mode toggles; exact
`\x1b[?2031l` (disable) followed by `\x1b[?2031h` (enable) sequence ordering across bidirectional mode switches;
same-root delayed query reply acceptance after a 120ms wait (artifact 31 does not observe timer completion before injection);
bidirectional mode switches (`regular -> fullscreen` and `fullscreen -> regular`) with an unsettled query pending
on the retiring root before each switch (queue depth is asserted during capture, not exported). Active-root consume
results reject old replies; instrumentation observes calls at the active terminal input-handler boundary, without
measuring physical bytes at retired roots. Subsequent terminal appearance notifications
(`\x1b[?997;1n` / `\x1b[?997;2n`) trigger a single query burst and settle foreground/background colors; ambiguity
control demonstrating unkeyed OSC 10/11 responses matching open pending queries FIFO; roundtrip rebind
idempotency maintaining strictly 1 active scheme listener and emitting exactly 1 query burst; and malformed,
unsupported, and unrelated CSI handling where invalid RGB responses mark targets replied without color data
and valid DA1 completes the query. Artifact 31 exercises no OSC 4 replies or duplicate replies. Its retired-root
timeout effect was originally inferred from source; artifact 32 below supplies the actual completion observation.
Artifacts 30/31 inherit historical manifest environment metadata rather than measuring the current host; COLORFGBG
was not scrubbed. Artifact 31's controlled mount adds children but omits production setLayoutRoot; protocol vectors
are the measured boundary. Its root-N owner strings are opaque run-order tokens. Pi allocates
replacement TUI instances on every switch while Pike reuses retained root allocations; and unkeyed VT color
replies lack transaction IDs so active roots match responses FIFO against pending queries.
Public C++ acceptance tests consume `product-theme-rebind-protocol.json` directly through real `InteractiveEngine`,
`AgentSession`, and `SettingsManager` in `TuiModeSwitchTest.cpp`, asserting ordered exact mode 2031 enable/disable
transitions across bidirectional switches, query reply drain on the product executor, unkeyed FIFO consumption,
and roundtrip listener idempotency. The companion `ThemeControllerTest.cpp` cases use actual regular/fullscreen
roots and the existing public terminal-color sink to compare exact foreground/background RGB values with
`settledColors`, distinguish two pending FIFO queries, and verify malformed controls leave completion pending
until DA1. Additional editor-border challenges use the #991 background-over-scheme precedence contract to make
ignored replies observable; these contrasting challenges extend the frozen scenarios rather than replaying their
initial appearance literally. The callback phase assertions record Pike's delivery sequence around the frozen
query phases; artifact 31 does not export a complete frozen callback timeline. The initial retired-root empty
timeout callback versus silent cancellation difference motivated artifact 32 and the explicit renderer-handoff
completion path below. Generated system palettes remain outside this consumer proof.

`product-theme-rebind-lifecycle.json` (artifact 32) adds two bounded observations using the actual frozen controller,
real regular/fullscreen roots, the real `InteractiveMode.prototype.switchTuiMode`, and delegated method wrappers
installed before start/rebind. The retired query is observed pending before the switch. Awaiting its actual query
promise and controller apply records empty colors, one change callback, two replacement-root invalidations, and
one replacement-root render request, with exact ordered events, queue/listener snapshots and raw write window.
The window ends at controller completion; scheduled painting after that boundary is not counted as absent.
The replacement then receives a scheme report, emits the full 19-command query, and accepts exact new RGB.
The same-root scenario reports all 16 distinct OSC 4 colors. A contrasting duplicate for target 0 cannot complete
the query early or replace its first value; the eighteenth distinct color completes the query, and trailing DA1
drains its frozen queue entry. A subsequent empty timeout is observed through actual promise completion before
late input; late background applies while retaining the reported palette. This measures terminal palette reporting,
not generated system-theme colors. Public C++ sinks verify the palette/duplicate/late values. The initial frozen-one-versus-Pike-zero
retired completion RED is retained in development evidence; renderer handoff now explicitly preserves unresolved
query completion at its original deadline, while ordinary stop/destruction retains callback cancellation.

Artifact 32 fixes COLORFGBG to `0;15`, exports current controlled inputs separately from the inherited historical
envelope, and deep-copies event snapshots with local serials and logical owners. The controlled context supplies
settings/session fields, mounts children and setLayoutRoot, and stubs extension rebind and unrelated Azure catalog
loading; no provider or AgentSession runs in the frozen capture. Retired roots remain alive through observers and
pending timers. Input observations describe terminal handler dispatch, not physical device bytes. The one-off
`capture/recapture-product-theme-rebind-lifecycle.mts` appends this family and its coupled source hashes, preserving
all prior 31 artifact bytes and baseline metadata. Pike's renderer-switch stop explicitly detaches unresolved queries
from old-root input routing while allowing their existing query-owned timer to deliver retained partial colors to
the live controller. No second timer is created. Ordinary stop and destruction still cancel; controller disposal
suppresses callbacks and reports already posted to the host executor. Tests cover partial results, original deadlines,
completed-query duplication, stale replies, queued reports across rebind, and disposal. Frozen queue retention until
DA1 is an internal difference from Pike detachment; generated system-theme repaint policy remains outside this proof.
`capture/product-theme-rebind-protocol-scenarios.mts` defines the scenarios;
`capture/recapture-product-theme-rebind-protocol.mts` is the append recipe (same frozen revision
`7c10bd4337495ee613f2224843ecdf349b80d1df`, same `capturedAt`, same `environment`, every other artifact
byte-identical, manifest 30 → 31).

For the frozen fullscreen exit output policy (#1019, spec #946, group E20),
`fullscreen-exit-output.json` (artifact 33) records seven scenarios of product exit presentation
and terminal restoration through frozen `InteractiveMode.prototype.stopInteractiveTui`,
`formatResumeCommand`, and `VirtualTerminal`:
- `fullscreen-transcript-exit` observes fullscreen exit under `fullscreenExitOutput = "transcript"`:
  the alternate buffer is left, `switchTuiMode("regular", false, false)` transfers the mounted regions
  to a regular `TuiMainScreen` without starting a replacement renderer, `renderNow()` outputs the
  transcript once to the main buffer, and `MainScreen.stop({ preserveScreen: false })` leaves the cursor
  below the rendered lines. The terminal start count does not advance; the terminal stop count advances
  by 2 (once during switch, once on final stop).
- `fullscreen-resume-hint-exit` observes fullscreen exit under `fullscreenExitOutput = "resume-hint"`:
  the mode remains fullscreen, the alternate buffer is exited with `preserveScreen: true`, no transcript
  or document lines are emitted, the main buffer's original content is preserved byte-identically,
  and the terminal stop count advances by 1.
- `regular-mode-exit` observes exit from an explicit regular session: no mode switch occurs,
  `preserveScreen: false` leaves the incremental conversation lines and final shell cursor in place,
  and `fullscreenExitOutput` setting variations produce identical regular-screen outcomes.
- `empty-transcript-exit` observes fullscreen transcript exit with an empty conversation document,
  rendering only the remaining mounted dock/editor rows into the main buffer.
- `long-transcript-exit` observes fullscreen transcript exit when conversation messages exceed the
  constrained viewport height (12 message lines in an 8-row terminal): regular line-flow rendering
  ensures all 12 rows are emitted into the scrollback history rather than clipped to the viewport window.
- `overlay-dismissal-exit` observes that active overlay entries are dismissed before mode switching and
  stop, ensuring overlay modals do not leak into the terminal main buffer or transcript.
- `format-resume-command-matrix` records six distinguishing cases of `formatResumeCommand`:
  default session directories, custom session directories (unquoted, space-containing, and
  single-quote-escaped), unpersisted sessions (null), missing session files (null), non-TTY stdout (null),
  and the exact dimmed notice styling `\x1b[2mTo resume this session:\x1b[22m <command>\n`.

Artifact 33 preserves all prior 32 artifact bytes, baseline metadata, and generator source digests.
`capture/fullscreen-exit-output-scenarios.mts` holds the scenario bodies;
`capture/recapture-fullscreen-exit-output.mts` is the append recipe (manifest 32 → 33).
The captured scenarios execute over `VirtualTerminal` and instrumented product harnesses;
actual subprocess, PTY, and signal exit lifecycles are validated in C++ consumer test suites.


For #967, the same screen-state artifact also records seven independent SelectList scenarios:
cyclic selection/callback order, the centered five-row window, description allocation at
widths 40/41/60, wide-glyph truncation including the exact ANSI reset, unbound-key
non-mutation, value-prefix filtering/selection reset, and Unicode description normalization.
The filtering observations distinguish value prefixes from labels and fuzzy subsequences,
retain dotted-I expansion and Greek final-sigma context, reject `STRASSE` for `Straße`, and
include supplementary case mapping. They record filtered identities through public selection
APIs, plus silent repeated-filter resets and clearing. Description cases cover NBSP,
ideographic spaces, BOM, repeated CRLF and whitespace-only content.
`capture/select-list-scenarios.mts` executes the frozen component directly;
its source digest is recorded alongside the main generator digest. Public C++ caller tests
additionally verify key admission/propagation, which upstream's void `handleInput` cannot
report directly.

Capture explicitly, from a clean frozen pi checkout with its existing dependencies:

```bash
PI_CHECKOUT=/absolute/path/to/pi PI_BASELINE=pi-v1.0.4 \
  /absolute/path/to/pi/node_modules/.bin/tsx fixtures/pi-tui/capture/capture-named-tui.mts
python3 scripts/tui/evidence.py verify --baseline pi-v1.0.4
python3 scripts/tui/evidence.py read --baseline pi-v1.0.4 --artifact input.json
```

A linked worktree must supply `PI_CHECKOUT`; the convenience default is the primary clone's
sibling `../pi`. Capture requires Linux x86-64/glibc, checks the exact revision and clean
source tree before importing pi, fixes terminal capability inputs, and executes upstream
modules rather than reading Pike output. It records Node/glibc versions and hashes the
capture generator. Extend that generator with each later ticket's independent scenarios
and artifacts using the same envelope; update the manifest and its registry pin through
capture, then implement the corresponding public C++ replay. Every bundle must retain
input, component, screen-state and capability-ledger families. Re-capture is explicit and changes timestamp
and provenance even if the observed values do not change.

The offline `verify`/`read` CLI never captures, imports pi, or selects historical files.
`read` emits the exact bytes already verified in memory, preventing a verify-then-reopen
race. Missing bundles, unknown names, wrong revisions, absent metadata, omitted families,
unrecorded artifacts and digest/size mismatches fail with no artifact bytes on stdout.
The actual named C++ consumers call this reader; verification is part of replay, not an
optional preparatory command.

```bash
python3 tests/tui/PiTuiEvidenceTest.py
cmake --build --preset vcpkg --target cch_tests_tui -j2
ctest --preset vcpkg -N -L issue947
ctest --preset vcpkg -L issue947
```

The older capture script and root JSON files below retain their original provenance and
remain explicit historical regression inputs. They cannot satisfy a named pi-v1.0.4 read.
The old reported exclusions/defaults are historical authority only; ADR 0067 governs the
new effort. The ADR is an accepted scope record, and the three initial observations are
runner acceptance, not complete behavioral parity.

For #1012, the named runner added `alt-screen-image.json` and `iterm2-image.json`; the other
families keep their bytes. `capture/alt-screen-image-scenarios.mts` drives pi's real `Image`,
`ScrollView`, `VStack`, `TuiAltScreen` and `VirtualTerminal`, records the write stream and reduces
every frame to the image commands it carried together with the row each one executed at, so a
re-ordered clear, a re-transmitted resource, a stale placement or a deletion aimed at another id
fails. The kitty scenarios use a 16x64 PNG whose four 16-pixel bands differ in colour:
`image-scroll-crop-round-trip` distinguishes the cropped source band at the window edge
(`y=48,h=16,r=1`) from a reduced destination height or an unmodified source,
`image-anchor-move-reuse` distinguishes the placement-only `a=p` reuse from a second transmission,
`image-resize-repaint` distinguishes a reflowed window that drops the placement entirely from one
that keeps a stale strip (the upload survives and returns as `a=p`),
`image-removal-session-teardown` distinguishes the redraw clear `d=a` from the teardown `d=A`, and
`image-decoded-bytes-eviction` / `image-offscreen-count-eviction` distinguish the per-id `d=I`
eviction under the decoded-byte and count caps. The iTerm2 family stays separate original evidence
for the frozen fullscreen fallback: one `[Image: [image/png] 16x64` fallback row and no image
command before or after `stop`. The replay renames the random kitty ids to first-appearance
indices, so identity is compared rather than the allocation.

## Historical scoped evidence

The committed evidence bundle for the pi-tui completion gate ([#386]), closing the scoped toolkit
phase of parity map [#2] (ADR 0035). Every snapshot below is compared byte-for-byte by tests in
this repository against the C++ `cch_tui` surface, so the gate's evidence is one checklist away.
TUI fixtures carry no credentials by nature; all data is dummy and all dimensions/environments are
deterministic (see [Sanitization rules](#sanitization-rules)).

## Pinned baseline and artifact

- **Frozen pi commit:** `83114817c68f5413e4d7ba6d7003ddc511cd31d2` (the parity map [#2] baseline).
  The local pi checkout is `../pi`; `pi:` references resolve from that root. The toolkit phase is
  pinned to this commit by ADR 0035 and re-verified here file by file (the baseline-comment sweep
  advances every `src/tui/` behavioral-baseline comment to it; ADR 0026/0028 User Bash pins and
  `src/coding_agent/tui/` comments are app-layer surface and stay for the pi-coding-agent phase
  audit).
- **Published artifact:** `@earendil-works/pi-tui@0.83.0`, the commit-correlated artifact
  reference at pi tag `v0.83.0`. Unlike the pi-ai gate there is no generated-data gap — the
  toolkit is pure source, so no shard hashes are required: the committed snapshots below are the
  artifacts, regenerated by the capture sidecar from the frozen checkout.

## Sanitization rules

- **Deterministic environment:** the capture script pins `HOME`/`USERPROFILE` to `/home/tester`
  and scrubs every terminal-emulator variable (`TERM`, `TERM_PROGRAM`, `KITTY_WINDOW_ID`,
  `WEZTERM_PANE`, `TMUX`, …) before importing pi sources, so image fallback text and capability
  detection never depend on the runner's real terminal. The C++ differential tests replicate the
  same pins (`tests/support/EnvVarGuard`, `set_image_capabilities`).
- **Deterministic dimensions:** every screen-state golden and every render corpus case fixes the
  terminal dimensions in the snapshot.
- **Dummy data only:** image payloads are fixed dummy base64 strings (`AAAA`, 8192 `A`s to cross
  the 4096-byte Kitty chunk boundary); file names are dummy paths. TUI fixtures carry no
  credentials by nature; no fixture value was copied from a live session.

## Fixture inventory

### Input decoding (`input-decode.json`) — fork-A evidence

The decode-at-edge differential corpus generated from pi `keys.ts` / `stdin-buffer.ts` at the
baseline. Every entry is `escape sequence → {id, eventType}` where the TS side is pi's own
`parseKey`/`isKeyRelease`/`isKeyRepeat` output and the C++ side is the typed decoder's
`key_id()`/event type:

- **`corpus`** — 140 sequences where both sides agree across the legacy table
  (`LEGACY_SEQUENCE_KEY_IDS`, shift/ctrl-modified forms, F keys, single-byte keys, ESC-prefixed
  combos), the modifyOtherKeys table (`CSI 27;m;k~`), and the Kitty table (CSI-u with
  modifiers/event types/alternate and base-layout keys, navigation/functional forms, keypad
  equivalents). `PiTuiDifferentialTest` byte-compares each.
- **`modeDependent`** — 23 ambiguous legacy sequences whose reading depends on pi's
  process-global Kitty flag (`\n` → enter/shift+enter, `\x1b\r` → alt+enter/shift+enter, ESC +
  letter/digit/symbol → alt+… / undefined, …). The C++ decoder is single-table and matches the
  **legacy column**; the kitty column is pinned for the record. This is the observable surface of
  the fork-A divergence (ADR 0035: "decoded-event divergence"): under an active Kitty protocol
  the terminal itself emits CSI-u sequences for those keys, so the mode-dependent re-readings are
  dead branches in the C++ single-table decode.
- **`divergences`** — 29 recorded identifier-level divergences with pi's output pinned: uppercase
  letters (pi parses `"A"` as raw text; the C++ decoder canonicalizes to the `shift+a`
  identifier while inserted text preserves the typed case via
  `detail::printable_text` — pi's own "shift+letter produces uppercase" contract) and unshifted
  uppercase codepoints (pi rejects them; the C++ decoder still decodes the uppercase character
  key — unreachable by any binding or printable path). `PiTuiDifferentialTest` pins the C++
  outcomes.
- **`discarded`** — 12 control/output sequences (CSI clears, OSC titles/progress, DCS/APC
  payloads, old-style and SGR mouse forms) that both sides drop.
- **`paste`** — bracketed-paste framing (`\x1b[200~ … \x1b[201~`, pi `stdin-buffer.ts`
  `BRACKETED_PASTE_START`/`END` with `terminal.ts` re-wrapping) with control-looking content
  (SGR, `CSI 27;5:3u`-shaped text, nested paste markers); the C++ decoder must emit exactly one
  `PasteEvent` with pi's content and line count.
- **`chunkSplits`** — chunk-split boundaries: sequences fed in 1–4 byte pieces must reassemble to
  the full-buffer decode (pi `stdin-buffer.test.ts` split expectations).

### Keybinding table (`keybindings.json`)

The assembled default table from pi `TUI_KEYBINDINGS` (`keybindings.ts` at the baseline) — all 39
entries with `defaultKeys` normalized to arrays and their descriptions. `PiTuiDifferentialTest`
compares the 32 assembled actions against
`builtin_tui_keybinding_definitions()` action-for-action (ids, keys, descriptions) and asserts the
other ids are never no-op bindings. `tui.input.copy` is recognized-but-unassembled
(`is_known_unassembled_tui_keybinding`). The six `tui.altScreen.*` entries in this historical
fixture predate pi v1.0.4 — frozen v1.0.4 moved them out of `TUI_KEYBINDINGS` into
`ALT_SCREEN_KEYBINDINGS` and changed defaults (`top`/`bottom` are `ctrl+home`/`ctrl+end`,
`keybindings.ts:208-211`) — so the differential test skips them and they are validated against the
frozen v1.0.4 source (`alt_screen_keybinding_definitions()`) and captured scroll-view behavior
instead. Within the toolkit, `tui.altScreen.pageUp`/`pageDown`/`top`/`bottom` are assembled by the
fullscreen keybinding manager (#1004); prompt navigation, half-page/line scroll, and transcript
search stay recognized-but-unassembled until their own slices.

### Terminal-image encoder (`image-encoder.json`)

Frozen pi encoder bytes (`terminal-image.ts`): Kitty placement payloads with `C=1`
(`encodeKitty` with `moveCursor: false`) for a single-chunk and an 8192-byte chunked payload
(4096-byte chunk boundary), iTerm2 `File=inline=1` payloads with and without a base64 `name`, the
OSC 8 `hyperlink` helper, and `imageFallback` outputs under both hyperlink capability states with
`~/` shortening and `pathToFileURL` percent-encoding. The C++ side compares
`detail::encode_terminal_image`/`hyperlink`/`image_fallback` bytes. The C++ surface always places
with a cell region, so pi's optional-dimension forms are not ported (terminal-owned placement per
ADR 0035 fork B).

### Utility corpus (`utils.json`)

`visibleWidth`/`truncateToWidth`/`wrapTextWithAnsi`/`sliceByColumn`/`stripTerminalSequences`
outputs for a fixed corpus (plain, ANSI-bearing, CJK, emoji/flag/combining, OSC 8, tabs).
`PiTuiDifferentialTest` byte-compares `visible_width`, `truncate_text` (including pi's always-on
`\x1b[0m` ellipsis resets), `wrap_text`, `slice_by_column`, and `strip_terminal_sequences`.

The `wrap` corpus carries the #705 break-point cases: a CJK run starting behind a partially
filled line for every script in pi's `cjkBreakRegex` set (Han; Hiragana with attached combining
marks; Katakana; Hangul; Bopomofo), a mark-bearing cluster whose base is outside the set, a
styled span whose style closes inside its physical line, and an over-long token after a partial
line. It also carries the #707 reset cases: an open foreground style across CJK breaks and an
open underline across a word break, both reaching the final line, so the corpus pins that only
underline/hyperlink close at a break and the final line carries no reset. The #704 review fixes
add the `wrapSingleLine` push and trim cases: a whitespace-only prefix ahead of an over-long token
(plain, underline, and hyperlink), a control staged after that whitespace, the fit-break push
where the deferred whitespace already fills the width ahead of a word that would otherwise fit
(plain, wider, underlined, and control-staged, against the fitted line that keeps its whitespace),
the trailing-whitespace trim of a wrapped input line against a fitting input line that keeps its
whitespace, and the logical-newline cases where underline and hyperlink stay open.
The over-long whitespace-prefix cases cover underline, foreground, hyperlink, plain text,
a control attached to the following word, a fitting following word, and trailing whitespace.
They pin both the chunk boundaries and reset bytes; the focused width regression independently
checks that the originally reported underlined prefix cannot exceed the requested width.

**#704 review scope correction:** the original #705 statement that existing whitespace handling
already matched pi was disproved by the frozen oracle. The reviewed correction deliberately
retains pi's empty first row for `wrap_text("  ab", 3)` (`["", "ab"]`, rather than `["ab"]`).
This is an intentional whitespace behavior change within the text-parity fix, not a claim that
whitespace behavior is unchanged. The fixture baseline remains `83114817`; no layout or padding
policy is changed.

### Fuzzy corpus (`fuzzy.json`)

`fuzzyMatch` scores and `fuzzyFilter` rankings for a fixed corpus (exact-match bonus,
consecutive/word-boundary bonuses, alphanumeric swap fallback, whitespace/`/`-separated tokens).
Scores are IEEE-754 doubles computed in identical evaluation order on both sides and round-trip
exactly through JSON.

### Markdown corpus (`markdown.json`)

Rendered output lines of pi's `Markdown` component (baseline `components/markdown.ts`) for the
observable parity surface (ADR 0035): plain paragraphs, inline emphasis/strong/strikethrough/code,
**strict strikethrough**, fenced code blocks with border/indent, **streamed partial-closing-fence
trimming**, lists, blockquotes, horizontal rules, **capability-aware OSC 8 link rendering**, and
HTML passthrough. The #707 cases add pi's `defaultTextStyle.bgColor` backgrounded blocks — an
inline-styled wrapping paragraph and a fenced code block — with the recorded `background`
prefix/suffix. The recorded deterministic theme (a `styles` table in the snapshot) is rebuilt
as equivalent hooks by `PiTuiDifferentialTest`, including the composition pi performs internally:
link text gets `link(underline(…))` and blockquote text gets the quote style twice, and each
`background` case gets the recorded hook. Byte parity holds for every corpus case.

### Screen-state goldens (`screen-state.json`) — fork-B evidence

18 `VirtualTerminal` screen-state scenarios with the TS-side pin cited row-by-row: pi's own test
expectations (`tui-render.test.ts`, `tui-shrink.test.ts`, `tui-cell-size-input.test.ts`, the
overlay suite, `input.test.ts`, `select-list.test.ts`) transcribed as small line arrays, plus the
ADR 0037 main-screen scrollback-flow scenarios (full-buffer write, viewport-top tracking,
startup-header-scrolls-away, first-render-no-clear, resize clear-screen+scrollback, and
image-follows-content).
`ScreenStateGoldenTest` replays each scenario through the `VirtualTerminal` harness and
byte-compares the screen state (or forwarded-input outcome). The rows cover the renderer
(first/middle/last-line diff updates, clear on shrink), the cell-size response consumer (bare-ESC
forwarding, `CSI 16 t` consumption with cell-dimension update), overlay placement (bottom-right
anchor with an explicit viewport anchor — the C++ overlay anchor is caller-set, matching pi's
position options — and top-left stacking), the non-capturing overlay input pass-through,
component rendering (Input horizontal scroll with CJK, SelectList description normalization),
and the scrollback flow: the full composed buffer written with no viewport clipping (overflow
advances into the terminal's native scrollback), the tracked viewport top advancing as content
grows, the startup header scrolling away once the buffer grows past one screen, the first render
keeping pi's assumes-clean-screen semantics (no clear event), the height-change full redraw
emitting `\x1b[2J\x1b[H\x1b[3J` (clear screen, home, clear scrollback) so reflow starts clean,
and a Terminal-Owned Image Placement keeping its absolute row identity as its row scrolls into
scrollback.

### Capture sidecar (`capture/`)

`capture/capture-tui-snapshots.mts` regenerates every JSON fixture above from the frozen pi
checkout (mirroring `fixtures/pi-ai/capture/`): it refuses to run unless the checkout sits at
`83114817`, pins the deterministic environment before importing pi sources, derives the corpus
from pi's own tables, and asserts each corpus entry's pi parse matches the table it was built
from. Regenerate with:

```bash
../pi/node_modules/.bin/tsx fixtures/pi-tui/capture/capture-tui-snapshots.mts
```

`PI_CHECKOUT` overrides the default sibling `../pi`.

`capture/generate-cjk-break-ranges.mts` derives `src/tui/CjkBreakRanges.inc` (pi's CJK
line-break code points, #705) from the same frozen checkout with the same refusal guard: it
imports `cjkBreakRegex`, asserts its source text is the recorded one, and emits the ranges the
regex matches. The generated file carries its own provenance header and regeneration command.

## Manual evidence (automation-unreachable surfaces)

The following surfaces cannot be exercised deterministically through the `VirtualTerminal` seam
because they require a real emulator; they carry bounded manual evidence per the ADR 0035 gate.
Run each item against a real terminal and record the outcome in the gate report:

| # | Checklist item | Expected outcome | Real-terminal evidence |
| --- | --- | --- | --- |
| 1 | Kitty keyboard-protocol negotiation against a real emulator | Running the C++ TUI under Kitty reports the Kitty protocol (flags 7 = disambiguate\|events\|alternate-keys) and modified keys arrive as `CSI u` sequences (repeat/release event types observable with per-component opt-in) | Manual run under Kitty ≥ 0.31; negotiation ordering query → flags/device-attributes → modifyOtherKeys fallback |
| 2 | OSC 0 window-title visibility (`set_title`) | `Terminal::set_title` updates the emulator window title exactly once per call | Manual run; title text observed in the window chrome |
| 3 | OSC 9;4 progress visibility (`set_progress`) with the 1-second keepalive | Progress indicator appears in the emulator's taskbar/progress area and does not disappear while active (keepalive re-emits before the emulator timeout) | Manual run; indicator visible during a long operation |
| 4 | Synchronized output | Render batches are framed with `CSI ?2026 h/l`; no partial-frame flicker on redraw | Manual run; rapid renders show no tearing |
| 5 | Actual Kitty image display | `place_image` with the Kitty protocol displays a real PNG/JPEG/GIF/WebP in the target cell region with the cell-size math and animation-id reuse | Manual run with a real image file; fallback text appears when images are disabled |
| 6 | Actual iTerm2 image display | `place_image` with the iTerm2 protocol displays the image via `OSC 1337;File` | Manual run under iTerm2 |

The real-terminal checklist is bounded to these six items — everything else on the ADR 0035
surface is exercised deterministically by the differential tests, the lifecycle tests, or the
fail-closed Parity Architecture Gate (ADR 0039). No E2E (interactive-mode boot through an agent
turn) is attempted: that belongs to the pi-coding-agent gate.

**Recording.** The checklist above is the committed, bounded manual artifact of this gate; the
six expected outcomes are pinned here. Executing it requires a real emulator, so the run is the
maintainer's manual pass (same division as the pi-ai gate's optional `CCH_LIVE_KIMI=1` smoke).
Record each outcome in the gate report when the pass is performed, filling in the environment:

```
Real-terminal pass — date, emulator, version:
1. Kitty negotiation: PASS/FAIL + observed protocol report
2. OSC 0 title:      PASS/FAIL
3. OSC 9;4 progress: PASS/FAIL + keepalive observation
4. Synchronized output: PASS/FAIL
5. Kitty image:      PASS/FAIL + protocol and fallback observation
6. iTerm2 image:     PASS/FAIL
```

## Capability-to-source checklist

One line per scoped capability, tying it to the frozen pi source, the C++ surface, and the
committed evidence. The lifecycle/ordering/cancellation evidence lands with the implementation
tickets T2–T9 ([#378]–[#385]); this gate re-checks the coverage rows below.

### Supported Capabilities

| # | Capability | Frozen pi source | C++ surface | Evidence (tests → fixtures) |
| --- | --- | --- | --- | --- |
| 1 | Terminal seam: raw mode, bracketed paste (`?2004`), SIGWINCH refresh, Kitty negotiation (flags 7) with modifyOtherKeys fallback, split-response buffering with pi's 150 ms fragment timeout, synchronized output, `Expected`-carrying capability/mode reporting, caller-supplied fds, serial worker, drain-before-exit | `packages/tui/src/terminal.ts` | `Terminal.hpp`, `ProcessTerminal.hpp` (+`VirtualTerminal.hpp`), `src/tui/ProcessTerminal.cpp`, `KeyboardProtocol.*` | `ProcessTerminalTest` (11 rows: mode restore, rollback on partial startup, synchronized-output conservatism, drain), `TuiTest` `"Terminal seam records title and progress through VirtualTerminal"` |
| 2 | Terminal additions: `set_title` (OSC 0), `set_progress` (OSC 9;4 with pi's 1-second keepalive), `drain_input` (pre-exit Kitty key-release drain) | `interactive-mode.ts` call sites at `83114817` (`setTitle`/`setProgress`/`drainInput`) | `Terminal.hpp`, `ProcessTerminal.cpp`, `VirtualTerminal.cpp` | `ProcessTerminalTest` `"Process Terminal writes OSC 0 window titles"`, `TuiTest` `"VirtualTerminal stop clears an active progress indicator"`, `"VirtualTerminal drain input is a started-gated no-op"` |
| 3 | Input protocol layer (fork A): typed `KeyEvent`/`PasteEvent` decode at the terminal edge with pi's full sequence-table coverage (legacy, modifyOtherKeys `CSI 27;m;k~`, Kitty CSI-u with event types, shifted/base-layout alternate keys, lock-mask filtering, non-Latin safeguards); `parse_key_id`/`key_id`/`matches_key` implement pi's `modifier+key` grammar with alias canonicalization; press/repeat/release with per-component opt-in; bounded paste (1 MiB) | `packages/tui/src/keys.ts` | `Keys.hpp`, `src/tui/InputDecoder.*`, `Keys.cpp` | `KeysTest`, `TuiTest` input rows, `PiTuiDifferentialTest` → `input-decode.json` (140-corpus byte-compare, mode-dependent legacy column, 29 pinned divergences, paste framing, chunk splits) |
| 4 | Keybindings: the 30 assembled `tui.*` actions (21 `tui.editor.*`, 3 `tui.input.*`, 6 `tui.select.*`) with pi's default keys at `83114817`; immutable resolved registry, one startup read, replace-all-defaults, empty-array unbind, bounded redacted diagnostics; `tui.input.copy` + six `tui.altScreen.*` recognized-but-unassembled | `packages/tui/src/keybindings.ts` (`TUI_KEYBINDINGS`) | `Keybindings.hpp`/`.cpp`, `docs/keybindings.md` | `KeybindingsTest`, `PiTuiDifferentialTest` → `keybindings.json`, the Parity Architecture Gate (ADR 0039; user story 40) |
| 5 | Editor stack: multiline grapheme-segmented editing, paste-marker-aware word wrap, paste markers with `expanded_text`, undo, kill ring (yank/yank-pop), word navigation, jump-to-char, keybinding-table dispatch, cursor-location reporting, **history** (`addToHistory` on every submit path with cursor-boundary up/down recall), visible-line window `max(5, floor(rows × 0.3))`, `paddingX`, `autocompleteMaxVisible` (default 5, clamp 3..20); `EditorOptions::disable_submit` removed | `packages/tui/src/components/editor.ts`, `interactive-mode.ts` `addToHistory` call sites | `Editor.hpp`, `src/tui/Editor.cpp` | `EditorTest` (41 rows incl. `[issue379]` history rows: submit-path recording, 100-entry bound, cursor-boundary recall, undoable recall), `TuiTest`/`OverlayTest` cursor rows |
| 6 | Autocomplete: async debounced abortable provider contract, `AutocompleteItem`/`AutocompleteRequest`/`AutocompleteSuggestions`/`SlashCommand` values, `CombinedAutocompleteProvider` semantics (slash commands + `@`/`#` fd-backed filesystem completion with quoted-path handling, `.git` exclusion, graceful `fd` absence → empty suggestions, fuzzy ranking, abort awareness), `applyCompletion` text surgery | `packages/tui/src/autocomplete.ts`, `interactive-mode.ts` wiring | `Autocomplete.hpp`, `src/tui/Autocomplete.cpp` | `AutocompleteTest` (9 rows: debounce/abort ordering, stale-response rejection by generation, fd-walk abort, slash/argument completion), `EditorTest` `[issue383]` rows |
| 7 | Fuzzy as a public module: subsequence scoring with consecutive/word-boundary bonuses, matched-index reporting, ranking filter | `packages/tui/src/fuzzy.ts` (pi: 4 consumers) | `Fuzzy.hpp`, `src/tui/Fuzzy.cpp` | `FuzzyTest` (20 rows), `PiTuiDifferentialTest` → `fuzzy.json` |
| 8 | Single-line `Input` component (pi-canonical header name): horizontal scroll, bracketed paste, kill ring, undo, word navigation, submit/escape sinks, built from the private editor primitives exactly as pi's `components/input.ts` composes its editor | `packages/tui/src/components/input.ts` | `include/cch/tui/Input.hpp`, `src/tui/Input.cpp` | `InputTest` (13 rows), `SettingsListTest` `[issue384]` rows, `ScreenStateGoldenTest` `component-input-cjk` |
| 9 | `SettingsList` search rebuilt on the `Input`/`fuzzy` seams | `packages/tui/src/components/settings-list.ts` | `SettingsList.hpp`/`.cpp` | `SettingsListTest` (10 rows: search editing/paste through Input, ranking, hints registry) |
| 10 | TUI core: `Component` (`render → Expected<RenderResult>`), `Container`, `Focusable` with out-of-band `cursor_location()` (recorded divergence from pi's in-text `CURSOR_MARKER`), owned `Overlay` model (nine anchors, percentage sizing, margins/offsets, stacking, eligible/blocked/resume focus-restore), main-screen differential renderer under the ADR 0037 main-screen scrollback flow (pi `TuiMainScreen` parity: full composed buffer written with no viewport clipping so overflow advances into the terminal's native scrollback, tracked viewport top over the buffer, line-flow + first-diff differential updates, hardware cursor positioned relative to the viewport, width/height-change full redraw emitting `\x1b[2J\x1b[H\x1b[3J`, first render "assumes clean screen", Termux height-change special-case recorded as not applicable, clear-on-shrink, render coalescing at pi's 16 ms minimum, `CSI 16 t` cell-size consumption) | `packages/tui/src/tui.ts`, `tui-main-screen.ts`, the overlay suite | `Tui.hpp`, `Overlay.hpp`/`.cpp`, `Container.hpp`/`.cpp`, `Component.hpp`, `VirtualTerminal.*` | `TuiTest` (29 rows), `OverlayTest` (22 rows), `ContainerTest` (13 rows), `RenderDifferentialTest` (15 rows incl. the scrollback-flow rows), `ScreenStateGoldenTest` → `screen-state.json` (18 scenarios) |
| 11 | Terminal image (fork B): per-emulator capability detection with the tmux probe, Kitty/iTerm2 encoding behind terminal-owned `place_image`/`remove_image`, PNG/JPEG/GIF/WebP dimension sniffing, cell-size math (9×18 px default, `CSI 16 t` updates), resource-id/revision reuse for animation, public OSC 8 `hyperlink`, pi-exact fallback text (`[Image: path [mime] WxH]`, `~/` shortening, `file://` linking) | `packages/tui/src/terminal-image.ts` | `TerminalImage.hpp`, `src/tui/TerminalImage.cpp`, `Image.hpp`, `src/tui/Image.cpp` | `TerminalImageTest` (21 rows), `ImageTest` (18 rows), `ProcessTerminalTest` `[issue385]` rows, `PiTuiDifferentialTest` → `image-encoder.json` |
| 12 | Width/truncate/wrap/slice/strip public utility subset | `packages/tui/src/utils.ts` (`visibleWidth`, `truncateToWidth`, `wrapTextWithAnsi`, `sliceByColumn`, `stripTerminalSequences`) | `Utils.hpp`, `src/tui/Utils.cpp` | `UtilsTest` (34 rows), `UnicodeWidthTest` (26 rows), `PiTuiDifferentialTest` → `utils.json` |
| 13 | Component set: `Box`, `Text`, `Spacer`, `SelectList` (layout options, theme hooks), `Markdown` (tolerant parser retained as recorded divergence; streamed-fence trimming, strict strikethrough, code-block border/indent, capability-aware links are the byte-parity surface), `Image`, `Loader` (injected `AnimationTimer` retained as recorded divergence; 80 ms braille semantics), `CancellableLoader` (Escape via `tui.select.cancel` aborts; explicit state enum, completion/cancellation sinks), `TruncatedText` hard-cut default | `packages/tui/src/components/` (`box.ts`, `text.ts`, `spacer.ts`, `select-list.ts`, `markdown.ts`, `image.ts`, `loader.ts`, `cancellable-loader.ts`, `truncated-text.ts`) | the component headers under `include/cch/tui/` | `SelectListTest`, `MarkdownTest`, `LoaderTest`, `TruncatedTextTest`, `PiTuiDifferentialTest` → `markdown.json` |
| 14 | `VirtualTerminal` retained as the public protocol-aware test double (deliberate C++ idiom) and the gate's rendering-evidence harness | pi's equivalent is test-local (`test/virtual-terminal.ts`) | `VirtualTerminal.hpp`/`.cpp` | `TuiTest` rows, `ScreenStateGoldenTest` → `screen-state.json` |

### Lifecycle/ordering/cancellation coverage re-check (ADR 0035 gate tier 2)

Every deterministic surface carries lifecycle/ordering/cancellation tests landing with T2–T9; the
gate re-checks the rows:

- **Terminal start/stop RAII and mode restoration** — `ProcessTerminalTest` `"Process Terminal
  restores raw paste cursor and pending render modes"`, `"Process Terminal rolls back raw input
  after partial startup failure"`, `"Process Terminal runs a minimal TUI shell and restores during
  unwinding"`, `"Concurrent external and sink stops restore without deadlock"`.
- **Keyboard-protocol negotiation ordering (query → flags/device-attributes → modifyOtherKeys
  fallback)** — `ProcessTerminalTest` `"Process Terminal enables and restores the keyboard
  fallback"` + negotiation rows.
- **Kitty press/repeat/release filtering with per-component opt-in** — `TuiTest` `"Tui filters key
  releases unless the focused Component opts in"`, `"Tui decodes Kitty alternate keys keypad and
  modified navigation"`.
- **Paste-bound enforcement** — `TuiTest` `"Tui bounds large paste payloads and preserves
  deterministic metadata"`, `"Tui abandons incomplete paste safely when the terminal flushes
  input"`, `"Tui delivers bracketed paste atomically"`.
- **Editor lifecycle (focus/cursor, submit routing, history recall, undo/kill-ring ordering,
  paste-marker expansion)** — `EditorTest` (history `[issue379]`, undo/kill-ring `[issue48]`,
  paste-marker rows).
- **Autocomplete debounce/abort ordering with stale-response rejection** — `AutocompleteTest`
  `[issue383]` rows (`"aborts the fd walk on stop request"`), `EditorTest` `[issue383]` rows
  (`"rejects stale autocomplete responses by generation"`, `"aborts an in-flight autocomplete
  request when superseded"`, `"escape-cancel keeps a late in-flight result from reopening the
  menu"`).
- **Overlay stacking with the eligible/blocked/resume focus-restore policy** — `OverlayTest`
  (`"Overlay z-index controls input routing order"`, `"Hide and restore overlay with focus
  fallback"`, `"Remove overlay disposes it and fallback focus works"`, `"Stacked overlays render
  in z-order"`).
- **Loader clock and cancellable-loader abort ordering** — `LoaderTest` (`"Loader requests
  renders for animation and stops without busy waiting"`, `"CancellableLoader arbitrates
  completion and cancellation exactly once"`, `"CancellableLoader resolves concurrent terminal
  outcomes exactly once"`).
- **Image place/remove/revision lifecycle including resize** — `ImageTest`, `TerminalImageTest`,
  `ProcessTerminalTest` `[issue385]` rows.
- **Render coalescing and clear-on-shrink** — `TuiTest` `"Tui clears and repaints unchanged
  retained content"`, `RenderDifferentialTest` shrink rows, `ScreenStateGoldenTest` shrink rows.
- **Drain-before-exit** — `ProcessTerminalTest` drain rows, `TuiTest` `"VirtualTerminal drain
  input is a started-gated no-op"`.

### Deferred Capabilities (absent from the surface — no placeholders, no compatibility shims)

Verified absent-with-no-placeholder by the fail-closed Parity Architecture Gate (ADR 0039): any
reintroduced unclassified/unknown target, root, or source fails configure/build (spec user story
40), and by the header inventory:

- The entire alt-screen/viewport half: `TuiAltScreen`/`ViewportTUI`/`setLayoutRoot`, the layout
  engine and `LAYOUT_NODE` protocol, `ScrollView`, `Stack`/`HStack`/`VStack`,
  `AltScreenFlashContainer`, SGR mouse/wheel/scrollbar interaction, app-owned text selection,
  OSC 8 click-through, OSC 133 prompt navigation, the six `tui.altScreen.*` actions' active
  behavior and `tui.input.copy`'s active behavior (no `uiMode` setting, no UI-mode CLI flag, no
  layout seams) — the seven ids are recognized-but-unassembled, never no-op bindings.
- The `EditorComponent` extension interface and the TUI extension input-listener chain
  (`addInputListener` consume/rewrite).
- The Apple-Terminal Shift+Enter synthesis, darwin native-modifier polling, and the Windows
  VT-input native helper (all outside the supported Linux platform; pi itself fails closed
  without its platform prebuilds).
- Live `?2031` color-scheme notifications and the TUI-level background/scheme query APIs
  (startup OSC 11/`?997` detection stays Supported); the `Marked` re-export; `getOsc8LinkAtColumn`.
- The diagnostic surface (`onDebug`, `fullRedraws`, `PI_TUI_WRITE_LOG`) and pi exports with no
  scoped consumer (public `StdinBuffer`, terminal-colors parsers, the keyboard-negotiation
  parser).
- Interactive-mode application components (model selection, login presentation, footer/status,
  chat UX), interactive-mode boot, and E2E — pi-coding-agent phase.

## Final classification

Every capability scoped by ADR 0035 is either a **Supported Capability** carrying the evidence in
the checklist above (differential byte-compare, screen-state goldens, lifecycle tests, or the
bounded manual checklist), or a **Deferred Capability** verified absent by the fail-closed Parity Architecture Gate (ADR 0039).
There are no partial placeholders and no compatibility shims.

Two alignment defects surfaced by the gate were fixed and pinned while building the evidence: the
decode layer dropped `$`-final legacy shift sequences (`\x1b[2$` … `\x1b[8$`) and the NUL byte
(`\x00` → ctrl+space), and typed uppercase letters lost their case at insertion
(`detail::printable_text`); all three now carry regression rows in `TuiTest`/`EditorTest` and are
pinned by the input-decode corpus. The `truncate_text` ellipsis now always carries pi's `\x1b[0m`
resets. Recorded renderer-side divergences (documented in the README rows above): pi's multi-line
`visibleWidth` sums graphemes across newlines; C++ `visible_width` matches that sum (issue #956); the
`truncate_text` fits path closes underline/hyperlink before padding where pi pads inside the
still-open span (`truncateToWidth("\x1b[4mabc", 8, "", true)` is `"\x1b[4mabc     "` in pi and
`"\x1b[4mabc\x1b[24m     "` here); a zero-width control staged immediately after visible content
lands at the end of the pushed line in `wrap_text` where pi stages it on the continuation line
(`wrap_text("中文\x1b[31mABCDEFGHIJ", 4)` is `["中文\x1b[31m", "\x1b[31mABCD", …]` here and
`["中文", "\x1b[31mABCD", …]` in pi), so the rows render identically; and `truncate_text` keeps
the control immediately preceding the first non-fitting grapheme before the always-on reset where
pi drops it (`truncate_text("\x1b[4ma\x1b[31mbcdef", 4, "...")` is
`"\x1b[4ma\x1b[31m\x1b[0m...\x1b[0m"` here and `"\x1b[4ma\x1b[0m...\x1b[0m"` in pi), invisible
and pre-existing. The `truncate_text` fits path is pre-existing, invisible in every shipped
surface, and the spec's "Padding is not changed" decision excludes it.

Full test suite: **1695 test(s), 0 failure(s)** at the #386 gate (see the gate report below).

## Gate report

### Per-capability evidence map

- **Input decode (fork A)** — `PiTuiDifferentialTest` `input-decode` rows (140 corpus + 23
  mode-dependent + 29 divergences + 12 discarded + 3 paste + 5 chunk splits) + `KeysTest`/
  `TuiTest` input rows.
- **Keybindings** — `PiTuiDifferentialTest` assembled-table row + `KeybindingsTest` + the
  recognized-but-unassembled rows in `KeybindingsTest`/`PiTuiDifferentialTest` and the Parity
  Architecture Gate (ADR 0039).
- **Encoder bytes** — `PiTuiDifferentialTest` `image-encoder` row + `TerminalImageTest` (21) +
  `ImageTest` (18).
- **Utility/fuzzy/markdown surfaces** — `PiTuiDifferentialTest` `utils`/`fuzzy`/`markdown` rows +
  the module test suites.
- **Renderer (fork B)** — `ScreenStateGoldenTest` (18 scenarios incl. the ADR 0037
  scrollback-flow rows) + `RenderDifferentialTest` +
  `TuiTest` + `OverlayTest`.
- **Lifecycle/ordering/cancellation** — the re-check table above (T2–T9 tickets).
- **Deferred absence** — the Parity Architecture Gate (ADR 0039; user story 40).

### Residual notes

1. **The real-terminal checklist is bounded and recorded as a fill-in artifact, not executed in
   CI** — items 1–6 of the manual-evidence table require a real emulator; the expected outcomes
   and the negotiation/ordering expectations are pinned in this README, and the executing
   maintainer records the per-item outcome in the gate report using the fill-in format above
   (deterministic environment only — no live credentials exist in TUI surfaces).
2. **Fork-A mode-dependent legacy readings** are pinned with both pi columns in
   `input-decode.json`; the C++ single-table decoder matches the legacy column by design (ADR
   0035 decoded-event divergence).
3. **Markdown and wrap byte-parity are scoped to the recorded parity surface**: markdown corpus
   cases cover streamed-fence trimming, strict strikethrough, code-block border/indent,
   capability-aware links, and the #707 backgrounded blocks (headings are excluded — pi composes
   per-level bold/underline where the C++ role hook is single-composition); wrap break points,
   including the CJK break opportunities, the over-long-token path, and the #707 styled-across-a-
   break reset placement, are byte-parity in the corpus.
4. **Overlay anchors are caller-set in the C++ model** (the Tui does not default them to the
   viewport); the golden rows set the anchor explicitly, matching pi's position options.
5. **No live-terminal or network validation** — all evidence is deterministic per the repo
   validation policy.

### Handoff surface to pi-coding-agent (confirmed)

The renamed-header toolkit contract the pi-coding-agent phase consumes for interactive-mode
assembly, model selection, login presentation, and status/cancellation UX:

- `Terminal.hpp`/`ProcessTerminal.hpp` (+ `set_title`/`set_progress`/`drain_input`),
  `VirtualTerminal.hpp`.
- `Keys.hpp` (typed events and the `modifier+key` grammar), `Keybindings.hpp` (resolved immutable
  registry + builtin table), `docs/keybindings.md` (the frozen 30-action table).
- `Editor.hpp` (history, async autocomplete), `Autocomplete.hpp` (provider contract,
  `SlashCommand`, combined provider), `Fuzzy.hpp`, `Input.hpp` (single-line component).
- `Tui.hpp`/`Overlay.hpp`/`Component.hpp`/`Container.hpp` (main-screen renderer, owned overlays,
  core primitives) and the component headers (`Text`, `TruncatedText`, `Markdown`, `Image`,
  `SelectList`, `SettingsList`, `Loader`, `CancellableLoader`, `Style`) plus the public
  width/wrap/truncate utility header `Utils.hpp`.
- The phase must not expect any alt-screen/viewport API, `EditorComponent`, extension input
  listeners, live `?2031` notifications, or a `Marked` equivalent; re-evaluating any of them
  requires a new map decision (ADR 0035).

[#2]: https://github.com/lanshengzhi/cpp-coding-harness/issues/2
[#376]: https://github.com/lanshengzhi/cpp-coding-harness/issues/376
[#378]: https://github.com/lanshengzhi/cpp-coding-harness/issues/378
[#379]: https://github.com/lanshengzhi/cpp-coding-harness/issues/379
[#380]: https://github.com/lanshengzhi/cpp-coding-harness/issues/380
[#381]: https://github.com/lanshengzhi/cpp-coding-harness/issues/381
[#382]: https://github.com/lanshengzhi/cpp-coding-harness/issues/382
[#383]: https://github.com/lanshengzhi/cpp-coding-harness/issues/383
[#384]: https://github.com/lanshengzhi/cpp-coding-harness/issues/384
[#385]: https://github.com/lanshengzhi/cpp-coding-harness/issues/385
[#386]: https://github.com/lanshengzhi/cpp-coding-harness/issues/386
