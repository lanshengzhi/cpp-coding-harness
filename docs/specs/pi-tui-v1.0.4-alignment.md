## Problem Statement

Pike users encounter terminal interactions that differ from the selected pi release: non-Latin Kitty input loses its original character, Chinese path completion can list the wrong directory, pasted controls leak printable tails, overlays misroute input or misplace the cursor, and component caches can show stale text. Markdown tables and math do not render equivalently. The reusable TUI Toolkit also lacks the full-screen layout, scrolling, mouse, selection, live-color, and extension-UI capabilities available in pi v1.0.4.

Pike integrators must compensate for missing toolkit responsibilities such as first-frame scheduling, immediate input painting, and coordinate composition. Existing pi differential fixtures describe an older release and cannot demonstrate alignment with v1.0.4. Historical exclusions and local terminal-writing paths would otherwise preserve these differences during isolated fixes.

The target is pi v1.0.4 at the Named Baseline `pi-v1.0.4`, exact revision `7c10bd4337495ee613f2224843ecdf349b80d1df`, on Pike's Supported Platform: Linux x86-64 with glibc. The target covers the reusable toolkit and necessary Native TUI/CLI integration, with equivalent capability availability, defaults, caller responsibilities, and observable outcomes. It does not make pi the architecture authority for unrelated packages.

## Solution

Provide a complete Linux TUI Toolkit with a common TUI interface, regular and fullscreen renderers, composable layout and input capabilities, and the product entry points needed to use them. Match the frozen release's default fullscreen mode and regular/fullscreen switching, including configuration, settings controls, exit output, transcript navigation, theme changes, clipboard, and images.

Keep idiomatic C++ ownership, typed input, immutable binding snapshots, and out-of-band cursor/image values, while making the toolkit preserve the same screen, text, input, lifecycle, and asynchronous outcomes. One renderer owns terminal output; remove the old direct Editor local-echo/dock path and regular startup DSR dependency. Build and accept complete slices against an explicitly frozen pi evidence bundle.

## User Stories

1. As a terminal user, I want Pike to start in fullscreen by default as pi v1.0.4 does, so that the selected baseline's layout and navigation are available immediately.
2. As a terminal user, I want to select regular or fullscreen through configuration and the CLI, so that my chosen mode is applied with the same precedence and defaults as pi.
3. As a terminal user, I want to switch modes from settings while retaining my text, conversation, focus, and component state, so that changing presentation does not restart my work.
4. As a terminal user, I want mode switching to handle active overlays as pi does, so that a dialog cannot lose its input or escape its lifecycle.
5. As a terminal user, I want fullscreen exit output to honor transcript or resume-hint settings, so that the restored main buffer contains the expected output.
6. As a regular-mode user, I want the initial frame to start where the shell left the cursor without a DSR response, so that shell content and history are preserved at startup.
7. As a regular-mode user, I want growth, shrink, width/height changes, and exit to match pi's scrollback behavior, so that terminal history and the next shell prompt remain predictable.
8. As a fullscreen user, I want fixed editor/footer regions and an independently scrolling transcript, so that reviewing history does not displace prompt entry.
9. As an integrator, I want both renderers to share a common TUI interface and explicit fullscreen capabilities, so that composition can change mode without duplicating focus and input logic.
10. As an integrator, I want to add, remove, and clear children and overlays safely, so that dynamic composition has the same usable operations and lifetime outcomes as pi.
11. As a user, I want opening, hiding, restoring, focusing, and explicitly unfocusing overlays to route input and restore focus like pi, so that visible dialogs and temporary replacement UI do not swallow the wrong keys.
12. As a user, I want default, percentage, anchored, offset, margin, and responsive overlay placement to match pi, so that dialogs remain visible and cover the intended region at narrow sizes.
13. As an IME user, I want the hardware cursor to follow the final nested or scrolled input position, so that the candidate window appears at the text I am editing.
14. As an IME user, I want hardware-cursor visibility control to behave as pi does, so that terminals requiring a visible cursor remain usable.
15. As a keyboard user, I want non-Latin Kitty input to preserve its actual character while retaining base-layout shortcut matching, so that typing and shortcuts both work.
16. As a keyboard user, I want legacy, modifyOtherKeys, Kitty press/repeat/release, timeout, and duplicate-input behavior to match pi, so that terminal protocol differences do not alter my input.
17. As a user, I want complete mouse/control packets and split terminal replies to remain separate from printable text, so that coordinates or negotiation bytes are never typed into my prompt.
18. As a user, I want paste normalization, CSI-u controls, tabs, newlines, and file-path spacing to match pi, so that copied content becomes the intended prompt.
19. As a user, I want paste collapse thresholds, displayed counts, and expansion to match pi, so that non-ASCII content is not prematurely collapsed or miscounted.
20. As a user, I want pastes larger than 1 MiB to be preserved rather than silently truncated, so that a toolkit-specific limit cannot alter my submitted content.
21. As an editor user, I want editing, undo, kill/yank, word movement, character jumps, and atomic paste markers to retain pi's results, so that familiar editing remains reliable.
22. As an editor user, I want Enter, backslash-Enter, temporary submit disabling, prompt clearing, and history recall to match pi, so that submission and multiline entry do not conflict.
23. As an editor user, I want viewport height, padding, scrolling, and paging to follow terminal dimensions as pi does, so that large prompts use the expected amount of space.
24. As a completion user, I want slash, skill, file, quoted, wrapped, whitespace, and CJK contexts to trigger and apply suggestions consistently, so that paths and commands are completed without corrupting their prefixes.
25. As a completion user, I want ranking, tie-breaking, cancellation, debounce, and stale-result rejection to match pi, so that the first suggestion and delayed results remain predictable.
26. As a completion user, I want menu item counts, descriptions, selection windows, styles, and configurable visible limits to match pi, so that suggestions remain readable.
27. As a single-line input user, I want prompt, placeholder, cursor, horizontal scrolling, paste, and raw-LF submission to match pi, so that searches and dialogs behave consistently.
28. As a user changing keybindings, I want dispatch, hints, help, and local selectors to observe the same updated binding context, so that a reload cannot leave conflicting shortcuts.
29. As a user, I want a modified Text child or background to update a containing Box without special host repair, so that the screen reflects the current component state.
30. As a user, I want whitespace-only, empty, padded, narrow, and truncated content to take the same rows and columns as pi, so that layout does not grow or fail unexpectedly.
31. As a user, I want SelectList and SettingsList navigation, filtering, descriptions, hints, selection by id, and submenu transitions to match pi, so that settings and choices behave consistently.
32. As a user, I want Loader and CancellableLoader messages, wrapping, animation, and cancellation outcomes to match pi, so that background work stays legible and cancellable.
33. As a reader, I want headings, links, quotes, lists, code fences, styling, and streamed Markdown behavior to match pi, so that assistant and tool text is presented consistently.
34. As a reader, I want GFM tables to have the same borders, column allocation, wrapping, and narrow-width behavior as pi, so that structured output stays readable.
35. As a reader, I want inline, display, unsupported, and incomplete streamed LaTeX to follow pi's rendering and fallback behavior, so that formulas are useful without losing source text.
36. As a user of non-ASCII content, I want width, wrapping, slicing, truncation, and fuzzy matching to produce pi's public results, so that text and selection align with the frozen baseline.
37. As a toolkit caller, I want retained fuzzy-index helpers to report original UTF-8 offsets correctly, so that highlighting targets the original text rather than a folded buffer.
38. As an image viewer, I want capability detection, cell sizing, format dimensions, placement, removal, and resizing to match pi, so that images occupy the correct cells and do not leave stale graphics.
39. As an image viewer, I want non-PNG Kitty images to use a registered converter or pi-equivalent fallback, so that JPEG/GIF/WebP content does not disappear.
40. As a fullscreen image viewer, I want Kitty cropping and placement reuse while scrolling, and iTerm2 fallback where deletion/cropping is unavailable, so that repainting leaves no stale image strips.
41. As a theme user, I want reusable color parsing/conversion/mixing and palette-aware rendering, so that the same styles can be expressed on truecolor and indexed terminals.
42. As a theme user, I want startup, late, and live terminal color responses and automatic theme pairing to match pi, so that the display follows the terminal's colors.
43. As a fullscreen user, I want VStack/HStack sizing, nested ScrollViews, follow-end, and responsive visibility, so that fixed and scrollable regions coexist.
44. As a fullscreen user, I want wheel, trackpad, keyboard, chained overscroll, and configurable scrollbars, so that I can navigate long content naturally.
45. As a fullscreen user, I want mouse hit-testing, click focus, hover, drag capture, and clickable list/editor rows, so that pointer actions affect the intended component.
46. As a fullscreen user, I want text selection, edge auto-scroll, clipboard copy, link clicking, and right-click paste behavior to match pi, so that mouse interaction is complete.
47. As a fullscreen user, I want search, match navigation, OSC 133 prompt jumps, flash messages, and a scroll-to-end indicator, so that I can find and return to relevant transcript content.
48. As a clipboard user, I want supported Linux native/platform/remote paths to distinguish unavailable, empty, and failed transfers, so that copy and paste either work or report the expected outcome.
49. As a C++ extension author, I want ordered raw-input consume/rewrite listeners with reliable cleanup, so that terminal input can be customized without leaking handlers across reload or mode changes.
50. As a C++ extension author, I want a replaceable editor with inherited text, bindings, autocomplete, callbacks, appearance, and focus, so that a custom editor can participate in the real Native TUI lifecycle.
51. As an integrator, I want the toolkit to schedule the first frame, coalesce ordinary work, immediately paint keyboard updates, and cancel work at stop, so that every host receives the same render policy.
52. As a product maintainer, I want Session Projection consumption and accounting to remain separate from toolkit paint policy, so that streaming convergence is preserved without two schedulers or terminal writers.
53. As a maintainer, I want Linux-usable diagnostic and reusable helper capabilities equivalent to pi, so that behavior can be inspected without changing the renderer.
54. As a reviewer, I want every capability and reusable contract mapped to frozen pi evidence and a discriminating acceptance case, so that passing shallow checks cannot hide a remaining gap.
55. As a project owner, I want each implementation slice to adapt its actual consumers and remove replaced paths, so that phased delivery does not leave permanent duplicate behavior.
56. As a project owner, I want the final default enabled only when all included capabilities and product entry points are assembled and verified, so that partial delivery is never represented as complete alignment.

## Implementation Decisions

### Authority and capability accounting

- ADR 0067 is the accepted target. Historical subset restrictions and exemptions apply only where that ADR explicitly retains them. Linux capability inclusion is already decided and must not be reopened by an implementation ticket.
- The frozen revision and a separately captured TUI Evidence Bundle own comparison expectations. Each capture records its revision, source endpoints, scenario inputs, environment, dimensions, capture metadata, and artifact digests. A requested missing or wrong-version bundle is an error, not permission to fall back to older fixtures or Pike-generated values.
- Before accepting behavioral implementation, enumerate the entire frozen toolkit export/capability surface and the necessary product consumers. Every row is either an included capability with an equivalent C++ caller contract, a representation-specific operation covered by the chosen typed/metadata model, or an explicit exclusion stated below. The initial inventory is a starting point, not a complete proof of coverage.
- For each included row, record its owning package, upstream contract, product consumer where applicable, implementation slice, comparison scenario, and acceptance result. Do not mark unfinished included work Deferred to close this spec.
- Semantic Parity covers observable meaning/state independently of language shape. Capability availability, default values, caller responsibilities, and independently usable interfaces are additional requirements. Identical TypeScript inheritance or raw ANSI bytes are not blanket requirements.

### Owner packages and interfaces

- `cch_tui` owns reusable terminal input, render policy, renderers, focus/overlays, layout, toolkit components, text/Markdown/math utilities, colors/terminal queries, and clipboard/transcoding capability interfaces.
- Product frontends own Agent Session/Projection consumption, prompt admission, settings and CLI policy, theme selection/pairing, product selector composition, extension-UI host wiring, and composition of host adapters. Headless Session/Agent/AI owners acquire no frontend dependency.
- Introduce a common TUI interface with MainScreen and AltScreen implementations and a distinct fullscreen capability interface. Common operations cover lifecycle, dynamic children, focus, overlays, input listeners, terminal queries, diagnostics, and render requests. Fullscreen operations cover layout roots, scrolling/navigation, selection/search, and viewport-specific state. Shared behavior has one authority rather than duplicated state machines in both renderers.
- C++ interfaces retain ownership and explicit errors. Data crossing Owner Interfaces stays passive; scheduling/platform adapters expose narrow physical capabilities and hide their concrete dependencies. No new process-global mutable state, per-Owner event loop, reverse Owner dependency, installed SDK, or ABI promise is introduced.
- Concrete C++ names, method signatures, layout representation, image converter implementation, and math-rendering dependency are selected during the relevant slice only after the smallest necessary capability/cost probe. Required behavior and ownership are settled; dependency choice must use the supported pinned toolchain and strict no-exception configuration.

### Render scheduling and terminal output

- The toolkit owns first-frame scheduling, request coalescing, normal 16 ms pacing, immediate keyboard-update rendering, forced rendering, and stop/restart cancellation. A narrow injected scheduler supplies monotonic time, deferred execution, and cancellable timing on the existing Runtime Root. Test adapters provide deterministic time without leaking test mechanics into production policy.
- Model-stream/Projection delivery does not perform synchronous rendering. Product consumption/accounting and frozen transcript caches remain; product changes request toolkit rendering rather than starting a second paint scheduler. Reconcile counted/preview assumptions explicitly while preserving convergence and nonblocking input.
- Input admission and repaint remain distinct contracts. Claiming a key is not proof that visible state changed, and rendering cannot determine prompt/interrupt admission.
- Only the renderer path writes interactive output. Delete direct Editor terminal echo, its terminal/dock-offset surface, old dock RenderResult protocol, and corresponding product bypasses as actual consumers migrate. Fixed input/footer regions are fullscreen layout, not another terminal-writing path. Adapt useful regression scenarios to the new path rather than deleting their property coverage.
- MainScreen uses relative line flow with no startup DSR dependency. Preserve frozen pi startup, overflow, differential updates, shrink policy, resize, hardware cursor, render-state capture/restore, stop/preserve-screen, and image lifecycle outcomes. The small positioning probe settles only the unknown-origin starting case; it is not renderer acceptance.
- AltScreen owns an absolute fixed-height viewport, alternate-buffer acquisition/restoration, viewport differences, scroll state, and final exit presentation. Ordinary image/cursor metadata survives composition and is placed only after final coordinate conversion.
- Preserve raw/bracketed-paste/keyboard negotiation/progress/title/cell-size/synchronized-output/start-stop restoration contracts. Backpressure, partial-write recovery, and Close must not duplicate output, lose input, or leave terminal modes active. Relative rendering does not remove failure recovery requirements.

### Input, binding contexts, and editor contracts

- Demultiplex terminal replies and input at the frozen-equivalent stages. Ordered raw-input listeners can consume or rewrite applicable input before typed dispatch, with exact release/paste/order semantics. Removal, reentrancy, reload, stop, and renderer replacement cannot retain stale listeners. Typed values preserve shortcut identity separately from actual printable text.
- Match frozen input framing, fragment deadlines, legacy/modifyOtherKeys/Kitty sequences, alternate-layout rules, duplicate suppression, event-type filtering, and complete mouse/control packets. Mouse capability absence does not permit payload bytes to enter the editor.
- Preserve pasted content beyond 1 MiB, removing visible truncation. Chunking/backpressure controls transient resources; any explicit failure or cancellation is surfaced, not substituted with truncated successful input. Apply pi's newline/control/TAB/path normalization, UTF-16-equivalent public counting, collapse/expand, and undo behavior while storing text idiomatically in C++.
- Shared and local keybinding contexts are explicitly updatable over immutable resolved snapshots. Dispatch/help/hints/default tables/unbind/conflict handling and consumer updates converge on the same context; no process-global manager is required.
- Editor, Input, autocomplete, fuzzy, SelectList, SettingsList, Loader, and CancellableLoader interfaces provide the frozen reusable caller capabilities and defaults. In particular, restore temporary submit disabling, pi-equivalent history ownership, backslash-Enter, height-aware viewport/paging, configurable autocomplete visibility, Input prompt/placeholder style, and submenu navigation.
- Natural/forced completion, wrapper/quote/CJK boundaries, Unicode whitespace, slash arguments, skill bare-name priority, file tie-breaking, replacement-prefix preservation, directory quoting, debounce/abort and stale-result rejection match the baseline. Missing fd follows the actual frozen graceful-absence contract.
- Public Unicode width/ANSI/fuzzy/counting outcomes match pi even when internal UTF-8/grapheme representations differ. Do not carry full casefold/NFC or widest-line semantics forward as undocumented improvements. Retained fuzzy indices map to original UTF-8 offsets and remain consistent with the retained matching contract.
- Product-specific selector chrome, search composition, and extra shortcuts live in product composition and cannot consume keys the baseline toolkit would bubble. Retain VirtualTerminal and consumed C++ helper capabilities; remove duplicate behavior paths.

### Composition, overlays, layout, and pointer interaction

- Frame metadata or equivalent focus-path metadata expresses component-local cursor/image geometry. The toolkit owns translations through child stacking, padding, constrained layout, clipping, viewport scroll, overlays, and terminal positioning. Product callers cannot manually repair offsets; hidden/clipped cursors and images follow the frozen outcomes.
- Overlay handles provide frozen show/hide/temporary-hidden/focus/unfocus/bounds behavior, including explicit targets, empty focus, deterministic visual ordering, visibility changes, and eligible/blocked/resume transitions. Input follows resolved focus, not unconditional interception by every visible overlay.
- Default centering/width, absolute and percentage dimensions/positions, signed offsets, margins, responsive visibility, width coverage, and screen-height composition follow pi. No unsigned underflow, accidental offscreen 100% placement, or short-title exposure is permitted.
- VStack/HStack constrained sizing and unbounded render behavior, responsive entries, nested ScrollViews, follow-end, primary navigation, scrollbars and wheel chaining follow the frozen contracts.
- MouseRegion and component mouse interfaces preserve local/screen geometry, buttons/modifiers/click count, capture/focus/render flags, hit-testing from the composed frame, and nested routing. Lists/settings/Input/Editor support their frozen mouse operations.
- Fullscreen includes selection/edge auto-scroll/copy-on-select, clipboard copy, OSC 8 click-through, right-click paste, accelerated/auto wheel behavior, search/navigation/highlights, OSC 133 jumps, flash presentation, and scroll-to-end indicator. Regular mode continues to let the terminal own scrollback/mouse selection as pi does.

### Components, Markdown, images, and colors

- Dynamic composition and caches reflect actual child/background changes. Match empty/whitespace content, narrow padding, wrapping/truncation, selector windows, hint ellipses, Loader presentation/animation, and cancellable lifecycle outcomes.
- Markdown renders frozen heading/style composition, lists/quotes/links/code and streamed fences, GFM tables, inline/display math and pending/unsupported fallback. A C++ parser may differ internally; output/state contracts do not. Provide the reusable math operation, not only a Markdown-only hidden special case.
- Images provide dimension sniffing and caller-supplied/default dimensions, cell-size/capability overrides, sizing/fallback/link helpers and encoder contracts through equivalent C++ capabilities. Protocol-specific bookkeeping that exists only for pi's inline sequences is represented by the metadata lifecycle, not duplicated as placeholder helpers.
- Product composition registers an image converter through an injected context. Kitty non-PNG images convert to PNG when available and use frozen fallback on absence/failure; conversion cache and changed dimensions, including EXIF orientation, match visible behavior. Fullscreen Kitty scrolling crops/reuses/removes placements; fullscreen iTerm2 images fall back where deletion/cropping is unavailable.
- Reusable color values, parsing, RGB/OKLCH/OKHSL conversion, mixing, terminal-mode styling, and palette querying belong to the toolkit. Product Theme policy consumes startup/default/palette results, late replies, scheme subscriptions, automatic pairing, rebind and reload without parallel environment-only authority.
- Linux native clipboard interfaces distinguish unavailable, empty, and failed transfers and expose only operations available on that platform. Product adapters cover the applicable platform-command/native/remote OSC 52 branches, copy/link/paste callbacks, and error outcomes. Do not invent Linux file-path or native-write support that frozen pi itself leaves unavailable.
- Linux-usable diagnostics, write logging, counters, raw buffering/parsing and reusable helpers receive equivalent caller capability coverage. Representation-specific exports are explicitly mapped; excluded language/platform objects are not emulated.

### Product configuration and phased delivery

- Match `tuiMode` with fullscreen default and `--tui-mode regular|fullscreen` precedence; do not add historical `uiMode`/`--fullscreen` aliases merely because older decisions mention them. Match settings controls and persisted values for fullscreen exit output, scrollbar, copy-on-select, and wheel lines, including defaults/validation and runtime actions.
- Mode replacement preserves component state/text/focus, handles active overlays as frozen pi does, transfers regular render state where required, and rebinds terminal queries/theme/listeners. Fullscreen exit restores the main buffer and applies the configured output. One session/terminal/runtime lifetime survives a mode change.
- Provide a minimal real C++ extension-UI host for editor factories and terminal listeners, including registration/disposal/reload/mode-switch lifecycle. Transfer the relevant default-editor callbacks, bindings, appearance, autocomplete, text and focus. This is not a TS execution host or a toolkit-only demonstration.
- Implement blockers before their consumers. Evidence capture precedes acceptance. Input/utilities/components can form parallel lanes once their behavioral prerequisites exist; common runtime/geometry enables renderers, layout, overlays, and pointer behavior; the initial single-output-path migration uses the common toolkit scheduler and regular renderer to replace Editor bypasses, adapting current product callers with complete streaming/Busy/Close coverage before final mode integration. Fullscreen layout then carries fixed regions and extends that migration's coordinate/output evidence. Final product modes depend on assembled fullscreen, theme/clipboard/image/extension hosts, and the completed initial output-path migration. Final default activation and complete close-out follow those lanes.
- Keep every intermediate slice buildable and focused-testable, with actual consumers adapted and replaced paths deleted. Do not expose inert menus, flags, or host methods. Default fullscreen activates in final integration rather than becoming a placeholder early.

## Testing Decisions

### Primary seams

- The primary reusable behavior seam is the public TUI/Component interface over the existing Terminal/VirtualTerminal adapters. Drive actual composition, focus, input, mode-specific rendering, and clock events; observe screen cells/styles, cursor, scrollback, images, callbacks, ordering, and terminal mode transitions. Utility-specific tests use their existing public functions, not private implementation hooks.
- The product seam is the real CLI/Interactive Session Run and Native TUI lifecycle over deterministic Session/host adapters. Confirm user-reachable config/CLI/settings/extension flows with process or PTY tests where appropriate. Component goldens alone cannot establish product reachability or Close correctness.
- Use a deterministic scheduler adapter only at the chosen host execution capability; it must exercise the production toolkit policy, including input immediacy and canceled work. Existing Terminal, clipboard/platform adapters, and Session assembly seams remain preferred to new mocks within implementation layers.
- Real-emulator observations supplement those seams only for outcomes automation cannot establish. Record emulator/version, date, dimensions, protocol, inputs, result, and evidence artifact. Kitty-protocol and iTerm2-protocol outcomes and real IME positioning require this treatment; absence of a suitable emulator means the row is unverified, not silently accepted.

### Evidence and discriminating cases

- Capture frozen pi expectations independently of Pike with deterministic environments, dimensions, filesystem entries, themes and clocks where possible. Compare full relevant values and state transitions; normalize only the already authorized representation differences. Preserve links, styles, cursor, focus order, image geometry, error outcomes and product defaults rather than projecting them away to obtain a match.
- Every criterion includes a case a cheap stand-in would let through: an identifier match that loses insertion text, a column-positive cursor at the wrong row, a visible overlay that steals explicitly redirected keys, a cache that renders correctly once but not after mutation, a parser that accepts text without producing a table, or a flag that parses without affecting real product behavior. Select at least one such case per criterion when creating child tickets.
- The capability matrix below states required properties and representative distinguishing cases. It is a coverage floor; capture the complete included export/capability surface and additional frozen regressions discovered during implementation.

| ID | Capability and acceptance property | Representative distinguishing evidence | Prerequisite group |
| --- | --- | --- | --- |
| E01 | Frozen baseline and complete interface/capability accounting | Wrong/missing bundle fails; no fallback; every export/consumer classified with observable evidence | None |
| E02 | Lossless keyboard/framing/listener semantics | Kitty `ф` with base `a`; split legacy mouse payload; consume/rewrite/release/paste chain; late control replies; duplicate suppression | E01 |
| E03 | Paste preservation/normalization/counting | CSI-u Ctrl+j becomes LF; tabs/path spacing; 400 CJK characters; >1 MiB expansion and exact submitted text | E01, E02 |
| E04 | Unicode/ANSI/fuzzy results and original offsets | Indic spacing marks, multline width, OSC/ANSI slice boundaries, `ss`/`ß`, offset in `İa` | E01 |
| E05 | Binding context convergence | Replace/unbind/reload changes dispatch and help together; local context isolation; fullscreen defaults | E01, E02 |
| E06 | Editor/Input/autocomplete caller contracts | Backslash-Enter, disabled submit/history, 40-row viewport, menu count, placeholder/raw LF, `./中文/文`, skill/file ranking, canceled old results | E02–E05 |
| E07 | Basic composition/selector/loader behavior | Mutate Box child/background without host repair; whitespace/empty/narrow rows; unhandled key bubbles; chained submenu; long Loader cancellation | E04–E06 as consumed |
| E08 | Markdown/table/math fidelity | Styled streamed fences, narrow table cells, H1/H2 roles, inline/display/pending/unsupported LaTeX | E01, E04 |
| E09 | Shared TUI lifecycle and scheduler policy | First frame without explicit host paint; manual-clock coalescing; keyboard bypasses timer; stop/restart discards stale work; output Busy recovery | E01, E02 |
| E10 | Foundational cursor/image coordinate composition | Input after five text rows; component stacking/padding; explicit local-to-frame coordinates; hardware-cursor visibility; actual final cell | E07, E09 |
| E11 | Overlay lifecycle/layout/focus and coordinate integration | 20×10 overlay at 50% of 100×30; zero-anchor default; explicit unfocus target/null; hide/unhide/resize restores focus/order; declared width masks content; displaced nested cursor/image | E09, E10 |
| E12 | Regular renderer and shell history | Dirty-screen boot at unknown row without CPR; growth/shrink/resize/preserve-screen/exit; regular render-state transfer | E09, E10 |
| E13 | Fullscreen viewport/layout and coordinate integration | Alternate buffer restored; VStack/HStack bounds; nested scroll chains/follow-end; clipped/scrolled cursor and images use E10 transforms; fixed editor/footer remains correct during menu growth | E09, E10 |
| E14 | Pointer targeting and scrolling | Cached-frame hit-testing without rerender; click focuses correct nested input; capture survives drag; wheel targets/chains; scrollbar drag/track jump | E02, E11, E13 |
| E15 | Fullscreen selection/search/navigation | Edge auto-scroll retains selection; copy-on-select false; links/right-click; next/previous match; OSC 133 jumps; scroll-to-end and flash | E05, E14, E18 for platform actions |
| E16 | Images and transcoding | PNG and known/default dimensions; non-PNG converter absent/fails/succeeds; EXIF geometry; resize/removal; scrolled Kitty crop/reuse; iTerm2 fullscreen fallback | E10, E12, E13 |
| E17 | Colors/queries/Theme integration | RGB/OKLCH/OKHSL/mix outputs; split palette replies; query timeout followed by late update; live scheme and renderer rebind | E01, E09 |
| E18 | Linux clipboard/platform capability outcomes | No display/Wayland/X11/remote branch; empty versus unavailable versus failure; OSC 52 encoding bound; real copy/paste callback wiring | E01, E09 |
| E19 | Extension editor/listener product lifecycle | Actual C++ extension replaces editor/rewrites input; reload removes old handlers; restore transfers text/callbacks; mode switch rebinds once | E02, E05, E06, E09, E13 |
| E20 | Product configuration/modes/exit/default | No-option fullscreen; CLI overrides settings; persisted fullscreen controls; runtime switch preserves text/focus/session; active-overlay rule; both exit outputs; final streaming/fixed-region behavior | E11–E19 as consumed, E21 |
| E21 | Initial single-output-path migration and Projection convergence | Current regular-mode consumer uses toolkit renderer only; inputs while streaming/Busy; no direct Editor writes/dock protocol; Base/Patch converges; Close stops callbacks. Fullscreen fixed-region extension is accepted in E13/E20 | E09, E10, E12 |
| E22 | Diagnostics and remaining reusable contracts | Real debug/write-log/counters/buffer/helper calls behave correctly; all representation-specific exports accounted for | E01, E09, relevant lanes |
| E23 | Final delivery and removal | Complete matrix, public caller examples and real product flows, no incomplete included rows; old echo/dock/DSR paths absent with behavior preserved | All included rows |

These prerequisite groups guide ticket ordering and are not yet child tickets or a mechanically generated DAG. Split broad groups into complete tracer bullets rather than one issue per file. Resolve cross-lane wiring through common interface tickets and integration tickets, not hidden mutual dependencies.

### Prior art and validation

- Existing toolkit differential tests, screen-state goldens, renderer comparisons, editor/input tests, terminal-stream/ProcessTerminal PTY tests, and image lifecycle tests provide prior art. Reuse their public seams while replacing their old baseline authority for new acceptance.
- Existing Native TUI differential, interactive boot/rendering tests, frame ticker/termination tests, settings and autocomplete flows, Session replacement, and real process interactive tests provide product prior art. A product smoke with deterministic fake Providers must exercise the same production assembly as the released binary.
- Characterize a reported gap with a discriminating failing case before fixing it. Extend regression coverage rather than rewriting expected output from Pike to make a failure pass. Compare retained old five fixes against new upstream evidence to prevent regression.
- Focused Validation builds the owning test shard and runs the smallest verified CTest selection during each slice; interface/ownership/build changes also run architecture validation. Verify selected case names/counts before claiming success. Keep the host's aggregate build parallelism at or below the documented four-job budget.
- Run Full Validation before code delivery: incremental build plus complete unfiltered offline tests, including the architecture gate. Fresh Validation is required only for a new environment, pinned-dependency/toolchain/configure changes, or explicit request; image/math dependency changes may trigger it. Use the repository's supported pinned dependencies and strict no-exception mode.
- Deterministic clock tests establish ordering/pacing without wall-clock sleeps. Performance claims require separate recorded measurements; timings must follow the repository's sanitizer/quarantine rules and cannot replace functional assertions.
- Final acceptance requires the complete capability/interface matrix, frozen differential results, appropriate process/PTY coverage, dated emulator evidence, architecture/Full Validation results, and proof that superseded output/positioning paths are physically removed. Included failures remain open with their blocking tickets; no scope reclassification closes them implicitly.

## Out of Scope

- Native macOS or Windows support, Darwin modifier/Apple-Terminal synthesis, and Windows VT platform helpers. Linux branches that frozen pi itself reports unavailable retain that outcome.
- Loading or executing pi TypeScript/JavaScript extension files. The C++ extension-UI host is included; a new language-execution host is not.
- Re-exporting Marked or TypeScript parser/token/runtime objects that C++ callers cannot consume. Equivalent Markdown/math/input/diagnostic behavior and Linux caller capability remain included.
- Global redesign of Agent, AI, Session storage, provider/auth behavior, configuration namespace, or the Product Architecture Contract beyond exact TUI-related boundary amendments required by this spec.
- An installed TUI SDK, stable ABI, TypeScript method-name/namespace/inheritance translation, or a general guarantee of byte-identical ANSI for observably equivalent rendering.
- Permanently retaining the old local-echo/dock path, dual regular positioning strategies, or process-global mutable state as compatibility workarounds.
- Treating unsupported terminal protocols as supported or inventing platform operations absent from frozen pi; preserve its explicit fallback/capability outcomes instead.
- UI redesign, new branding, arbitrary Unicode improvements that alter the frozen result, a line-count reduction goal, or speculative performance thresholds.
- Advancing the reference beyond the frozen revision during this effort, weakening evidence normalization to hide mismatches, or declaring incomplete included work Deferred.
- Implementing all unrelated coding-agent feature/settings/selector differences merely because the toolkit changes. Necessary mode/theme/clipboard/image/extension wiring and directly affected consumers are included; other feature parity is separate.

## Further Notes

- Authority: accepted ADR 0067, the current-source gap inventory, and the regular unknown-origin positioning observation. The inventory distinguishes narrow runtime probes from source inspection; none of those observations constitutes Full Validation or complete renderer acceptance.
- The reference is the local pi checkout at the exact full revision stated above. Product v1.0.4 uses `tuiMode`, `--tui-mode`, and default fullscreen; older regular-default/uiMode notes are historical.
- The five previously reported v1.0.0 gaps already have fixes: wrapper completion, leading-space slash completion, ANSI slice start ordering, TERM ending `-direct`, and Kitty aspect sizing. Preserve them while expanding frozen evidence; do not create duplicate fixes from the old report.
- Existing issues need explicit absorption/disposition when splitting this spec: #831 has a footer-preservation regression worth retaining although its dock-only remedy is superseded; #811's later comment reports its original repro stopped reproducing; #809's old settings-omission rule is superseded only for capabilities included here; #749 remains an umbrella whose landed layout/renderer seams should be reused, not repeated. #830 names related renderer commit/rollback cleanup, but this spec does not assume it is a required blocker. Do not close or relabel those issues as part of spec publication.
- Every child ticket must state the frozen contract, user-observable outcome, owning interface, actual consumer migration, discriminating acceptance cases, validation selection, removal obligations, and native blocking dependencies. Story/group references carry context; they do not replace a self-contained ticket.
- Detailed dependency and adapter choices may be resolved by the implementing agent using focused probes within this accepted scope. A real conflict with the settled product behavior or an upstream-defect exception requires an explicit owner decision rather than an unrecorded divergence.
