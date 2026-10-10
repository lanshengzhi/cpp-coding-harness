# cch_tui versus pi TUI v1.0.4: gap inventory

## Scope, baseline, and evidence

This audit compares reusable toolkit responsibilities, interfaces, and observable behavior. Pike has substantial main-screen coverage, but does not currently match pi v1.0.4 across the supported editor, overlay, component, and rendering surfaces. Previously deferred full-screen and extension capabilities also remain differences under the requested broader comparison.

- Pike: `9c7a2724029f9757c345d4c528d7ea30b0fba7ba`, clean at audit start; scope is `src/tui/` and `src/tui/include/cch/tui/`.
- pi: `/home/lansy/Work/github/coding-agent/pi`, clean; HEAD and `refs/tags/v1.0.4` both resolve to `7c10bd4337495ee613f2224843ecdf349b80d1df`; scope is `packages/tui/`.
- Product frontend code is referenced only to identify ownership transfers. This report does not claim an interactive-product audit.
- Inputs: three independent source scouts, current-source C++ probes, direct Node v26 probes importing pi's TypeScript sources, and the upstream release diff.
- Pike links are relative to this document; pi links are absolute local paths. Line numbers refer to these exact working-tree baselines.

Evidence labels throughout:

| Label | Meaning |
| --- | --- |
| **Probe** | The stated narrow result was executed against current sources. Some rows explicitly distinguish executed Pike results from pi source expectations. |
| **Source** | Inspected implementation or interface establishes the difference; terminal behavior was not exercised. |
| **Recorded** | An existing decision or evidence note explicitly identifies a divergence or deferred capability. This does not approve every behavior of its implementation. |
| **Unclassified** | No specific decision was found in the reviewed records. Absence does not imply approved exclusion. |

The decision reference is [ADR 0035](../adr/0035-own-the-scoped-pi-tui-toolkit-capabilities-for-the-three-provider-paths.md:15); the preceding inventory is [the v1.0.0 report](phase1-cch-tui-pi-v1.0.0-gap-inventory.md:1). Neither changes the requested comparison baseline to an older release.

## Architecture and responsibility mapping

| Area | Pike | pi v1.0.4 | Assessment |
| --- | --- | --- | --- |
| Root and composition | Final `Tui`, separate owned root children and overlay compositor; `Component::render` returns `Expected<RenderResult>`. | `TuiBase extends Container`, specialized `TuiMainScreen` and `TuiAltScreen`; components return line arrays. | Representation and ownership differ; full-screen specialization is absent. [Pike Tui](../../src/tui/include/cch/tui/Tui.hpp:40), [Component](../../src/tui/include/cch/tui/Component.hpp:35), [pi root](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui.ts:495>). |
| Input boundary | Decode once into typed key/paste events; protocol state is per terminal; private stream decoder and bounded paste. | Raw strings flow to components and key matchers; exported `StdinBuffer`, process-global Kitty state. | **Recorded** C++ choice. Preserve shortcut identity and printable text separately; the current non-Latin loss below is a behavior fault. [ADR](../adr/0035-own-the-scoped-pi-tui-toolkit-capabilities-for-the-three-provider-paths.md:15), [pi printable decoder](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/keys.ts:1350>). |
| Keybinding state | Injected immutable resolved `KeybindingRegistry`. | Mutable global `KeybindingsManager`, with local managers also possible. | **Recorded** API choice, not itself a defect. [Pike](../../src/tui/Keybindings.cpp:60), [pi](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/keybindings.ts:1>). |
| Cursor and images | Out-of-band cursor query; protocol-neutral image sidecars, terminal-owned placement. | Cursor marker and encoded image sequences in render lines. | **Recorded** representation choice. Final cursor/image cell placement still needs equivalent outcomes. [Pike](../../src/tui/include/cch/tui/Component.hpp:38), [pi marker](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui.ts:191>). |
| Async and lifecycle | `Expected`, RAII, owned providers, sinks and stop tokens; injected animation timer. | Promises/AbortSignal, callbacks, extensible Editor, self-managed timers. | Idiomatic representations. They are not defects merely because signatures differ. [Pike Editor](../../src/tui/include/cch/tui/Editor.hpp:89), [pi Editor](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/editor.ts:373>). |
| Render scheduling | Host receives render-request sink notifications; toolkit coalesces invalidations. | Root owns timer scheduling and immediate input rendering. | Responsibility transfer with observable consequences for reusable hosts; detailed below. [Pike](../../src/tui/Tui.cpp:292), [pi](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui.ts:1029>). |
| Theme and clipboard | Theme and clipboard readers/writers live in product frontend. | Reusable package exports color utilities and a native clipboard seam. | Ownership difference; product adapters do not prove complete toolkit API equivalence. [Pike Theme](../../src/coding_agent/tui/Theme.hpp:41), [clipboard](../../src/coding_agent/tui/ClipboardReader.hpp:19), [pi exports](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/index.ts:13>). |

Pike's Editor additionally has borrowed-Terminal local echo, dock offsets, and a top-border hook corresponding to the product's custom editor; pi's reusable Editor refers to TUI and supports subclass overrides ([Pike options](../../src/tui/include/cch/tui/Editor.hpp:63), [pi Editor](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/editor.ts:373>)). This is another interface/responsibility difference to consider when aligning module shape.

Pike additionally exposes a protocol-aware `VirtualTerminal` and a viewport/dock render protocol. pi's regular mode has no equivalent dock sidecar contract; its full-screen layout is a separate model. These additions should be compared as reverse differences rather than missing pi features ([Pike render values](../../src/tui/include/cch/tui/Component.hpp:38), [terminal dock](../../src/tui/include/cch/tui/Terminal.hpp:175)).

## Input decoding and editing

### I1. Kitty printable text loses the original layout character

**Probe; unclassified behavior fault in a recorded typed-input design.** `ESC[1092::97u` becomes key `a` in Pike; pi's `decodeKittyPrintable` returns `ф`. Pike substitutes the base-layout key into the same field that downstream components insert. pi can use the base layout for shortcuts while preserving the printable code point.

Evidence: [Pike substitution](../../src/tui/InputDecoder.cpp:132), [insertion helper](../../src/tui/InteractionUtils.hpp:41), [pi shortcut matching](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/keys.ts:1212>), [pi printable decoding](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/keys.ts:1350>). ADR 0035 explicitly promises non-Latin safeguards; typed events do not excuse losing text.

### I2. Old mouse framing leaks coordinate bytes into text

**Probe on both framing implementations.** For `ESC[M !!`, Pike emits the three printable payload bytes: space, `!`, `!`. Its CSI scanner ends at `M`; the pi StdinBuffer probe emits a single complete mouse frame `ESC[M !!`, including its three payload bytes. This compares framing; it does not claim StdinBuffer itself discards the packet. Mouse interaction is deferred, but swallowing a mouse packet as a unit is independently relevant to ordinary input integrity.

Evidence: [Pike scanner](../../src/tui/InputDecoder.cpp:407), [pi completeness](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/stdin-buffer.ts:44>), [recorded completeness promise](../adr/0035-own-the-scoped-pi-tui-toolkit-capabilities-for-the-three-provider-paths.md:31).

### I3. Duplicate-character suppression has different coverage

**Source; unclassified.** Pike suppresses the following `a` after `ESC[97;1u`; pi's duplicate regex accepts the form without explicit `;modifier`, so that stream produces two characters there. Pike's suppression checks a single-byte printable sequence; pi also handles BMP non-ASCII characters.

Evidence: [Pike](../../src/tui/InputDecoder.cpp:845), [pi regex](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/stdin-buffer.ts:186>), [pi code point](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/stdin-buffer.ts:399>).

### I4. Backslash-Enter compatibility differs

**Source on Pike; Probe on pi.** With `hello\` in the editor, ordinary Enter submits in Pike. The pi probe produced `hello\n` without submission: pi removes the trailing backslash and inserts a newline. pi also provides a related backslash-Enter submit path when submit is rebound to Shift+Enter.

Evidence: [Pike submit dispatch](../../src/tui/Editor.cpp:1245), [pi normal path](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/editor.ts:907>), [pi rebound path](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/editor.ts:1352>). **Unclassified.**

### I5. Paste cleanup changes content and collapse thresholds

**Probe** for CSI-u and non-ASCII collapse; **Source** for tabs and path spacing. These are separate from Pike's recorded 1 MiB paste bound.

| Trigger | Pike | pi v1.0.4 |
| --- | --- | --- |
| Paste `a ESC[106;5u b` | `a[106;5ub`: strips ESC but leaks the sequence tail. | `a\nb`: restores CSI-u Ctrl+j to LF before filtering. |
| Paste `a\tb` | Retains TAB. | Expands TAB to four spaces. |
| Paste `/tmp/x` immediately after `foo` | `foo/tmp/x`. | `foo /tmp/x`. |
| Paste 400 copies of `中` | Collapses to `[paste #1 1200 chars]`. | Keeps the 400-character body. |

Pike counts UTF-8 bytes for both the 1000 threshold and the displayed “chars”; pi counts JavaScript UTF-16 units. This representation difference becomes a visible behavior difference for multibyte text. **Unclassified.**

Evidence: [Pike sanitizer/count](../../src/tui/TextBuffer.cpp:68), [collapse](../../src/tui/TextBuffer.cpp:366), [Editor paste](../../src/tui/Editor.cpp:758), [pi CSI-u and normalization](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/editor.ts:1259>), [pi threshold](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/editor.ts:1300>).

### I6. Editor height and completion menu differ

**Source; unclassified; conflicts with recorded alignment claims.** Pike defaults to five visible editing lines and PageUp/Down step by that limit. pi uses `max(5, floor(rows * 0.3))`; a 40-row terminal gets twelve lines. Pike's available-height setter does not update its visible-line limit.

Pike also implements its own completion rows: five total rows include the counter, leaving four items when ten suggestions exist. pi shows five items plus the counter through `SelectList`. Pike prefixes even file suggestions with `/`, uses `>` rather than `→`, appends descriptions inline, and follows a trailing selection window rather than pi's centered window. pi exposes configurable `autocompleteMaxVisible`; Pike lacks the equivalent getter/setter.

Evidence: [Pike options](../../src/tui/include/cch/tui/Editor.hpp:51), [layout](../../src/tui/EditorLayout.cpp:22), [paging](../../src/tui/Editor.cpp:1227), [pi height](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/editor.ts:537>), [pi menu](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/editor.ts:2239>), [pi list](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/select-list.ts:99>), [ADR claim](../adr/0035-own-the-scoped-pi-tui-toolkit-capabilities-for-the-three-provider-paths.md:33).

### I7. Input placeholder and raw-LF behavior differ

**Source; unclassified.** Empty Input with placeholder `Search`: Pike emits a reverse-video blank then dim placeholder; pi reverses the `S` itself. Pike consumes an extra column. pi accepts prompt and placeholder-style options; Pike fixes `> ` and dim styling.

pi's Input submits on raw LF even if submit is rebound or unbound. With Kitty active, Pike decodes LF as Shift+Enter and uses its binding registry, so default Input does not submit on that event.

Evidence: [Pike interface](../../src/tui/include/cch/tui/Input.hpp:24), [placeholder](../../src/tui/Input.cpp:235), [LF decode](../../src/tui/InputDecoder.cpp:324), [pi options](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/input.ts:16>), [LF handling](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/input.ts:117>), [placeholder](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/input.ts:420>).

### I8. Submission controls and history responsibility differ

**Source.** Pike removed `disable_submit` by decision; the rationale says pi has no analog, but current pi has `disableSubmit`. This is a **recorded choice requiring reassessment**, not an unexplained omission. Pike adds submitted text to history inside Editor; pi's reusable Editor leaves `addToHistory` to the host. That responsibility difference is not just a language representation.

Evidence: [decision](../adr/0035-own-the-scoped-pi-tui-toolkit-capabilities-for-the-three-provider-paths.md:26), [Pike history](../../src/tui/Editor.cpp:744), [pi flag](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/editor.ts:371>), [pi submission](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/editor.ts:1363>).

## Autocomplete and fuzzy utilities

### A1. File and skill completion ranking differs

**Source; unclassified.** Pike stably sorts fd results only by score and truncates to twenty, leaving ties in fd order. pi resolves ties by path depth, length, and locale name before truncation; an empty `@` query can therefore expose a different visible set. Pike matches slash commands uniformly by full name. pi preferentially matches `skill:review` by bare name `review`, then appends skills found only by full-name matching.

Ordinary leading-space slash completion is fixed, but Pike's ASCII-only trim still differs from JavaScript `trimStart` for Unicode whitespace.

Evidence: [Pike file sort](../../src/tui/Autocomplete.cpp:532), [commands](../../src/tui/Autocomplete.cpp:666), [pi file ties](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/autocomplete.ts:806>), [skills](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/autocomplete.ts:356>), [trim](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/autocomplete.ts:338>).

### A2. Fuzzy Unicode matching and original-offset reporting differ

**Probe; unclassified.** `fuzzy_match("ss", "ß")` matches in Pike and fails in pi. Pike uses full casefolding/NFC and byte positions; pi lowercases and scores UTF-16 positions. Non-ASCII matches and rankings can differ.

Separately, Pike's extra `fuzzy_match_indices("a", "İa")` returns `3`, although `a` starts at original UTF-8 byte offset `2`. The implementation builds an original-offset map but returns folded indices without using it. This is a **Pike extension contract fault**, not a missing pi feature; pi exports no matching-indices API.

Evidence: [Pike folding](../../src/tui/Fuzzy.cpp:14), [unused offset map](../../src/tui/Fuzzy.cpp:113), [returned indices](../../src/tui/Fuzzy.cpp:168), [public offset contract](../../src/tui/include/cch/tui/Fuzzy.hpp:25), [pi matching](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/fuzzy.ts:11>).

### A3. Non-ASCII paths are split at every UTF-8 byte

**Probe on both providers; unclassified.** With a dummy `中文/文档.txt` under the same base directory, natural completion of `./中文/文` produces no suggestion in Pike, while pi suggests `./中文/文档.txt`. For `./中文/`, Pike extracts `/` and lists root directories; pi preserves `./中文/` and completes the intended directory. Pike treats every non-ASCII byte as a delimiter; pi explicitly keeps CJK letters in words and paths, separating only whitespace and punctuation.

The Editor trigger path separately recognizes only ASCII spaces/tabs, so completion after CJK punctuation such as `请看，@文件` also lacks pi's trigger boundary semantics (**Source**).

Evidence: [Pike delimiters](../../src/tui/Autocomplete.cpp:105), [prefix extraction](../../src/tui/Autocomplete.cpp:583), [Editor trigger](../../src/tui/Editor.cpp:300), [pi Unicode boundaries](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/utils.ts:59>), [pi prefix extraction](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/autocomplete.ts:527>).

## Overlay, focus, cursor, and render runtime

### R1. Explicit overlay unfocus and restoration are not equivalent

**Source; unclassified behavior fault under the recorded owned-overlay model.** Pike routes input through visible overlays in reverse z order before consulting the focused base component. Capturing overlays consume non-release input unconditionally. pi routes to its resolved focused component and supports `handle.unfocus({target: base})`, with blocked/resume restoration state.

The comparison must use that explicit handle operation: simply calling pi `setFocus(base)` can permit automatic overlay focus restoration and is not proof that pi always leaves the base focused. Pike has no equivalent explicit handle operation; assigning base focus does not stop its visible capturing overlay from receiving keys.

Pike add/restore operations also do not focus the overlay; setting focus does not raise z order. pi show/unhide/focus operations acquire focus when eligible and update `focusOrder`. Equal Pike z indices use `std::sort`, without pi's monotonic focus-order tie-break.

Evidence: [Pike dispatch](../../src/tui/Tui.cpp:330), [overlay consumption](../../src/tui/Overlay.cpp:160), [restore](../../src/tui/Tui.cpp:106), [compositor order](../../src/tui/OverlayCompositor.cpp:414), [pi explicit unfocus](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui.ts:780>), [pi show/unhide/focus](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui.ts:718>), [pi restore states](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui.ts:591>).

### R2. Overlay position and sizing differ

**Probe on Pike; pi expected coordinates from source.** A 20×10 overlay in a 100×30 viewport at 50%/50% starts at `(50,15)` in Pike, versus `(40,10)` in pi: pi applies percentages to remaining space after subtracting overlay size. Pike Center without an anchor wraps unsigned coordinates to approximately `2^64-10` and `2^64-5`; pi uses signed arithmetic and clamps.

Caller-set anchors are **recorded**. Pike defaults TopLeft with zero anchor dimensions; pi defaults center and width `min(80, availableWidth)`. These defaults remain experience differences even when caller-set anchors are accepted. The underflow and percentage outcome are separate concrete faults.

**Source:** pi supports percentage width/maxHeight, signed offsets, independent row/col, and a visibility callback. Pike has absolute limits and viewport-bound visibility. pi deducts margins before sizing and covers the declared overlay width; Pike adds margins to placement and derives coverage from rendered text width, potentially exposing underlying text beside short content. pi pads overlay composition to at least terminal height; Pike extends only through actual overlay rows.

Evidence: [Pike options](../../src/tui/include/cch/tui/Overlay.hpp:76), [position arithmetic](../../src/tui/Overlay.cpp:196), [coverage](../../src/tui/OverlayCompositor.cpp:344), [pi options](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui.ts:245>), [pi layout](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui.ts:1204>), [pi padding](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui.ts:1387>), [recorded anchor choice](../../fixtures/pi-tui/README.md:380).

### R3. Local cursor coordinates are not transformed through composition

**Source; unclassified.** Pike asks the focused component for its local cursor directly. Child stacking translates images but not cursor rows; Overlay returns its child's cursor without preceding-child height, overlay position, margins, or viewport translation. pi scans its cursor marker after composition.

An Editor after five Text rows, or inside a displaced overlay, therefore lacks the coordinate translation required to place the hardware cursor/IME candidate window correctly. Existing tests checking only a positive column do not establish final screen position.

Evidence: [Pike root cursor](../../src/tui/Tui.cpp:386), [child composition](../../src/tui/Tui.cpp:234), [overlay cursor](../../src/tui/Overlay.cpp:191), [test](../../tests/tui/OverlayTest.cpp:410), [pi final scan](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui.ts:1442>), [main-screen cursor](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui-main-screen.ts:271>).

### R4. Toolkit scheduling moves to the host

**Source; responsibility difference.** Pike invalidation coalesces a pending render and invokes a host sink; input dispatch does not automatically invalidate/render, and start does not request the initial frame. pi owns its 16 ms scheduling policy, requests a first frame, and requests immediate rendering after input. Reusable Pike hosts must supply the missing policy to obtain the same behavior.

Do not interpret the application's 16 ms retry constant as a toolkit minimum-render cadence. No cadence measurement was made here. The fixture/ADR claim of built-in cadence equivalence needs reconciliation with the current ownership seam.

Evidence: [Pike start](../../src/tui/Tui.cpp:118), [invalidation](../../src/tui/Tui.cpp:294), [input](../../src/tui/Tui.cpp:330), [pi start](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui.ts:930>), [scheduler](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui.ts:1029>), [input rendering](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui.ts:1118>).

### R5. Main-screen lifecycle and update policy differ

**Recorded** absolute rendering/DSR anchoring versus pi relative cursor movement: [ADR 0041](../adr/0041-own-the-anchored-absolute-flow-model-for-the-tui-main-screen-renderer.md:1). **Source** consequences: Pike's 250 ms DSR timeout clears screen and scrollback; pi's initial stream needs no CPR response. Pike rewrites from first difference to buffer end with full-width padding; pi ends at the last changed row. ADR 0041 explicitly records this write policy and viewport-bounded tail clearing as intentionally retained, with pi's corresponding optimizations not ported. More rows may be written for an early spinner; no cost benchmark or performance regression was established.

Pike lacks pi's `clearOnShrink` option, hardware-cursor visibility switch, `stop({preserveScreen})`, and capture/restore render-state API. Pike generally clears removed tail rows and has fixed dock/image cleanup on stop. pi also has a Termux height-change exception. These are behavior/configuration differences, not merely ANSI byte differences.

Evidence: [Pike timeout](../../src/tui/ProcessTerminal.cpp:80), [fallback clearing](../../src/tui/ProcessTerminal.cpp:1228), [rewrite/shrink](../../src/tui/RenderPipeline.cpp:555), [stop](../../src/tui/RenderPipeline.cpp:79), [pi initial render](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui-main-screen.ts:277>), [shrink/Termux](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui-main-screen.ts:347>), [diff range](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui-main-screen.ts:489>), [state/lifecycle](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui-main-screen.ts:135>), [cursor switch](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui.ts:560>).

## Components, Markdown, and width utilities

| ID / evidence | Difference and practical effect | Sources / classification |
| --- | --- | --- |
| C1 **Probe** | Render Box containing Text `old`, set child text to `new`, render at the same width: Pike returns `old`; pi returns `new`. Pike caches before rendering children. Dynamic background closure changes also need explicit invalidation in Pike, while pi samples them. | [Pike cache](../../src/tui/Container.cpp:112), [pi child/background sampling](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/box.ts:118>). **Unclassified** update-semantics fault. |
| C2 **Probe / Source** | Whitespace-only Text produces three rows in Pike, zero in pi (probe). A default Box containing empty Text produces two padding rows in Pike, zero in pi (Pike probe, pi source). Pike rejects narrow widths incompatible with padding; pi clamps content width to one. | [Pike Text](../../src/tui/Text.cpp:63), [Box](../../src/tui/Container.cpp:96), [pi Text](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/text.ts:61>), [pi Box](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/box.ts:123>). Narrow rejection is an explicit [Pike test contract](../../tests/tui/ContainerTest.cpp:167); this does not approve the empty-content difference. |
| C3 **Source** | Loader hard-cuts a long message into one row; pi inherits Text wrapping/padding and can show multiple rows. | [Pike](../../src/tui/Loader.cpp:288), [pi](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/loader.ts:17>). Timer injection is **Recorded**; truncation instead of wrapping is **Unclassified**. |
| C4 **Probe / Source** | A two-column table probe produces one row of literal pipe text in Pike and five bordered table rows in pi. Tables are not enabled in Pike parser flags and table nodes map to Paragraph. pi lays out bordered, wrapped cells. | [Pike parser](../../src/tui/Markdown.cpp:322), [node mapping](../../src/tui/Markdown.cpp:170), [pi table](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/markdown.ts:852>). **Unclassified** capability difference. |
| C5 **Probe / Source** | The probe `$\frac{1}{2}$` stays literal in Pike and renders as `1/2` in pi. Pike has no math-render option/API; pi recognizes inline/block LaTeX, attempts rendering, preserves failed source, and handles pending streamed math. | [Pike interface](../../src/tui/include/cch/tui/Markdown.hpp:34), [parser](../../src/tui/Markdown.cpp:322), [pi math](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/markdown.ts:125>), [export](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/index.ts:110>). **Unclassified**, not inferred Deferred. |
| C6 **Recorded / Source** | Heading role composition differs: pi H1 adds bold/underline and other levels bold; Pike has one heading hook without level. | [Pike](../../src/tui/Markdown.cpp:706), [pi](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/markdown.ts:481>), [fixture exclusion](../../fixtures/pi-tui/README.md:375). |
| C7 **Probe** | `का` and `क्ष` each measure one column in Pike and two in pi. Pike's mostly maximum-codepoint cluster width omits pi's spacing-mark and post-mark consonant additions. Wrapping, slicing, cursor, and overlay widths inherit the difference. | [Pike](../../src/tui/UnicodeWidth.cpp:232), [pi](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/utils.ts:186>). **Unclassified** semantic difference; actual emulator cell widths were not measured. |
| C8 **Recorded / Probe** | `visible_width("a\nb")` is one in Pike, two in pi: widest-line semantics versus summing grapheme widths. Other recorded residuals involve ANSI placement at wrap/truncate boundaries, including closing styling before padding. | [Pike](../../src/tui/Utils.cpp:94), [pi](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/utils.ts:298>), [residual details](../../fixtures/pi-tui/README.md:324). Reassess when exact API/byte parity is required. |
| C9 **Source** | SettingsList lacks select-by-id and submenu completion with `navigateTo` that opens another submenu. Long rows/hints hard-cut where pi uses default `...`. | [Pike interface](../../src/tui/include/cch/tui/SettingsList.hpp:43), [row cut](../../src/tui/SettingsList.cpp:296), [pi selection](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/settings-list.ts:85>), [submenu](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/settings-list.ts:273>), [ellipsis](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/settings-list.ts:162>). **Unclassified**. |
| C10 **Source** | Pike SelectList adds embedded search, title/hint/border, page keys, raw j/k, and noncycling navigation; pi delegates such composition to hosts. Indicator width bounds also differ in narrow terminals. | [Pike extension comment](../../src/tui/SelectList.cpp:28), [search/chrome](../../src/tui/SelectList.cpp:125), [indicator](../../src/tui/SelectList.cpp:539), [pi](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/select-list.ts:104>). Reverse differences; page keys are explicitly an extension. |

Pike Box/Spacer share `Container.hpp`, own children privately, and lack single-child removal; pi splits their files and exposes child removal. Pike root has add-child but no root clear/remove interface; Container has clear. This limits runtime composition without being a language-level defect ([Pike Container](../../src/tui/include/cch/tui/Container.hpp:34), [root](../../src/tui/include/cch/tui/Tui.hpp:50), [pi Container](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui.ts:355>)). Spacer's ordinary nonnegative integer row behavior showed no source-level difference.

## Images, colors, native clipboard, and public exports

| Area / evidence | Difference | Sources / classification |
| --- | --- | --- |
| Kitty transcoding **Source** | Pike accepts PNG for Kitty and otherwise falls back. pi can register `setImageTranscoder` for JPEG/GIF/WebP-to-PNG, caches conversion, and updates dimensions after EXIF rotation. pi also falls back when no converter is registered; default unconditional non-PNG support must not be claimed. | [Pike](../../src/tui/TerminalImage.cpp:502), [pi registration/conversion](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/image.ts:25>). **Unclassified** capability; added upstream after v1.0.0. |
| Image dimensions **Source** | Pike validates PNG chunks/IEND and requires successful sniffing before creating a sidecar. pi accepts caller dimensions, uses a looser PNG header sniff, and defaults to 800×600 when sniffing fails. Pike has no external-dimensions parameter. | [Pike sniff](../../src/tui/Image.cpp:126), [placement gate](../../src/tui/Image.cpp:387), [interface](../../src/tui/include/cch/tui/Image.hpp:43), [pi constructor](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/image.ts:78>), [PNG sniff](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/terminal-image.ts:523>). Bounded-input hardening is documented locally; approval of all strict-sniff outcomes is **not established**. |
| Image helper exports **Recorded** | pi exports sniffing, encoding, IDs, cell sizing, deletion, render/capability helpers; Pike hides/reallocates many of these under terminal-owned sidecar placement. | [Pike subset](../../src/tui/include/cch/tui/TerminalImage.hpp:61), [pi exports](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/index.ts:126>), [ADR sidecars](../adr/0035-own-the-scoped-pi-tui-toolkit-capabilities-for-the-three-provider-paths.md:16). Do not count every hidden helper as a missing observable feature. |
| Colors **Source / Recorded ownership transfer** | pi exports RGB/OKLCH/OKHSL, parse/mix/styleText and related utilities. Pike toolkit mainly offers style hooks and terminal state; product Theme owns RGB/xterm values and live foreground/background. No complete equivalent for the exported color-conversion/mix API was found. | [Pike Style](../../src/tui/include/cch/tui/Style.hpp:10), [Theme](../../src/coding_agent/tui/Theme.hpp:41), [pi exports](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/index.ts:13>), [old ownership note](phase1-cch-tui-pi-v1.0.0-gap-inventory.md:65). Transfer to product ownership does not establish full capability parity. |
| Native clipboard **Source** | pi exports `getNativeClipboard`: asynchronous text/image reads, optional file paths and text write; Linux loads an X11 helper only with DISPLAY, other platforms use native helpers. Pike toolkit has no clipboard interface. Product `AsyncClipboardReader` exposes image/text reads but not the same file-path/availability interface. | [pi contract](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/native-platform.ts:9>), [loading](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/native-platform.ts:61>), [Pike product reader](../../src/coding_agent/tui/ClipboardReader.hpp:19). **Ownership difference**; exact native clipboard parity remains **Unclassified** here. Darwin/Windows helpers have separate recorded deferrals. |

## Previously deferred capabilities still absent in this comparison

These are **Recorded Deferred**, not newly discovered implementation faults. They remain substantive differences when the goal is broad experience/interface alignment. Their previous deferral must be distinguished from approval of unrelated faults in supported paths.

| Capability group | pi v1.0.4 seam | Pike decision evidence |
| --- | --- | --- |
| Alternate screen, layout root/node protocol, ScrollView, stacks, flash, scrollbars, search, OSC 133 navigation | [AltScreen](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui-alt-screen.ts:202>), [layout](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/layout.ts:16>), [ScrollView](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/components/scroll-view.ts:22>) | [Deferred inventory](../../fixtures/pi-tui/README.md:293). Pike bottom dock is not equivalent to this stack. |
| Normalized mouse/wheel, click focus, drag capture, selection, copy, link click-through | [mouse contract](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui.ts:19>), [selection](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui-alt-screen.ts:1456>) | [Inventory](../../fixtures/pi-tui/README.md:295). Legacy packet framing fault I2 remains independently relevant. |
| Replaceable EditorComponent; consume/rewrite input listeners | [editor seam](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/editor-component.ts:11>), [listeners](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui.ts:1052>) | [Inventory](../../fixtures/pi-tui/README.md:299). |
| Live `?2031` scheme subscription; TUI background/scheme queries, broader OSC palette APIs | [scheme/query path](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/tui.ts:941>), [terminal colors](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/terminal-colors.ts:1>) | [Inventory](../../fixtures/pi-tui/README.md:305). Pike startup OSC 11/`?997` appearance detection remains supported. |
| Apple-Terminal Shift+Enter, Darwin modifier polling, Windows VT native input helper | [native platform](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/native-platform.ts:55>) | [Inventory](../../fixtures/pi-tui/README.md:301), platform/subset decision. |
| Marked re-export, diagnostic logging/counters, public StdinBuffer and unconsumed parser exports | [package exports](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/index.ts:3>), [StdinBuffer export](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/index.ts:115>) | [Inventory](../../fixtures/pi-tui/README.md:306). Internal decoder functionality is assessed separately from export visibility. |

## Recheck of the five v1.0.0 findings

All five original concrete findings have current fixes and regression tests. They should not be repeated as open gaps. This audit inspected those fixes; it did not rerun their test suites.

| Original finding | Current Pike evidence | pi reference |
| --- | --- | --- |
| Wrapper-aware completion | [prefix stripping](../../src/tui/Autocomplete.cpp:576), [provider tests](../../tests/tui/AutocompleteTest.cpp:238), [Editor trigger test](../../tests/tui/EditorTest.cpp:1004) | [wrappers](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/autocomplete.ts:63>) |
| Leading-space slash commands | [trim/match](../../src/tui/Autocomplete.cpp:666), [prefix-preserving apply](../../src/tui/Autocomplete.cpp:777), [test](../../tests/tui/AutocompleteTest.cpp:124) | [slash matching](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/autocomplete.ts:338>) |
| ANSI slice start ordering | [pending ANSI flush](../../src/tui/Utils.cpp:442), [test](../../tests/tui/UtilsTest.cpp:497) | [slice](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/utils.ts:1263>) |
| TERM ending `-direct` | [detection](../../src/tui/ProcessTerminal.cpp:153), [test](../../tests/tui/ProcessTerminalTest.cpp:797) | [detection](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/terminal-image.ts:75>) |
| Kitty aspect-ratio rounding | [distortion sizing](../../src/tui/OverlayCompositor.cpp:183), [use](../../src/tui/OverlayCompositor.cpp:285), [test](../../tests/tui/OverlayCompositorTest.cpp:260) | [sizing](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/terminal-image.ts:466>) |

The exact upstream `refs/tags/v1.0.0..refs/tags/v1.0.4` TUI diff changes ten files, with 285 insertions and 44 deletions. Main changes are image transcoder registration/non-PNG fallback, full-screen Kitty placement metadata/cropping fixes, and Home/End binding adjustments ([changelog](</home/lansy/Work/github/coding-agent/pi/packages/tui/CHANGELOG.md:3>)). Most findings above predate this release window. Pike's regular line-start/end defaults already match current Home/Ctrl+A and End/Ctrl+E ([Pike bindings](../../src/tui/Keybindings.cpp:256), [pi bindings](</home/lansy/Work/github/coding-agent/pi/packages/tui/src/keybindings.ts:208>)); do not list them as a newly missing feature. Full-screen Ctrl+Home/Ctrl+End actions belong to the deferred stack.

## Evidence drift, priorities, and limits

The differential fixtures still freeze pi v0.83.0 at `83114817c68f5413e4d7ba6d7003ddc511cd31d2` ([README](../../fixtures/pi-tui/README.md:11)); their [capture script](../../fixtures/pi-tui/capture/capture-tui-snapshots.mts:60) rejects other upstream HEADs. Passing those fixtures cannot establish v1.0.4 parity. The README also retains the obsolete 150 ms fragment-timeout description, corrected in [ADR 0035](../adr/0035-own-the-scoped-pi-tui-toolkit-capabilities-for-the-three-provider-paths.md:11). Its manual-terminal checklist is a fill-in artifact, not a dated current-emulator execution record ([residual note](../../fixtures/pi-tui/README.md:369)).

Recommended sequencing is based on observable impact and existing seams, not a new scope approval:

1. **First: input integrity and coordinates.** Preserve Kitty printable characters and Unicode completion prefixes; consume complete legacy mouse packets; repair CSI-u paste cleanup, overlay percentage/underflow, cursor coordinate composition, explicit unfocus routing, and Box stale-child cache. Fix fuzzy original offsets as a Pike contract issue. Add discriminating regression cases to the owning tests.
2. **Next: supported UX equivalence.** Align paste counts/tabs/path spacing, Enter compatibility, editor height/menu, completion ranking, Input placeholder/LF, empty-component behavior, Loader wrapping, and SettingsList operations. Reassess disableSubmit/history ownership and document whichever behavior is chosen.
3. **Then: decide broader capability/interface coverage.** Tables, math, image transcoder/dimensions, colors/native clipboard, and the previously deferred full-screen/extensions/live-color stack require explicit scope classification. Keep representation choices separate from missing observable outcomes.
4. **Refresh evidence with the implementation plan.** Capture against exact v1.0.4 and expand cases that distinguish the reported faults. Reconcile claims about scheduling/focus/cursor behavior with their actual owning seams, then run focused toolkit validation and real-terminal cases appropriate to the changes.

### Executed probe ledger

These observations are supplied by the parent audit's probes. This documentation task checked the relevant implementations and citations; it did not rerun those executables.

| Case | Executed Pike observation | pi observation / evidence | Report item |
| --- | --- | --- | --- |
| Width of `का` | 1 | 2, executed | C7 |
| Width of `क्ष` | 1 | 2, executed | C7 |
| Width of `a\nb` | 1 | 2, executed | C8 |
| `ESC[1092::97u` | Key `a` | Printable `ф`, executed | I1 |
| `ESC[M !!` | Space, `!`, `!` events | One complete mouse frame, executed | I2 |
| Paste with `ESC[106;5u` | `a[106;5ub` | `a\nb`, executed | I5 |
| Paste 400 `中` characters | `[paste #1 1200 chars]` | 400 units, uncollapsed, executed | I5 |
| `ss` versus `ß` fuzzy match | true | false, executed | A2 |
| Original indices for `a` in `İa` | 3, despite original byte offset 2 | No pi indices API | A2 |
| Box child changes `old` to `new` | Returns old cached text | Returns new text, executed | C1 |
| Whitespace-only Text | 3 rows | 0 rows, executed | C2 |
| Box with empty Text | 2 rows | 0 rows, inspected | C2 |
| 50% position, viewport 100×30, content 20×10 | `(50,15)` | `(40,10)`, inspected | R2 |
| Center without anchor, content 20×10 | Unsigned wrap near `2^64-10`, `2^64-5` | Signed/clamped viewport centering, inspected | R2 |
| `hello\` then ordinary Enter | Direct submit path, inspected | `hello\n`, no submit, executed | I4 |
| Completion `./中文/文` | No suggestions | Completes `./中文/文档.txt`, executed | A3 |
| Completion `./中文/` | Prefix `/`, lists root directories | Preserves prefix, completes intended file, executed | A3 |
| Two-column Markdown table | One literal pipe-text row | Five bordered table rows, executed | C4 |
| Inline math `$\frac{1}{2}$` | Literal source | `1/2`, executed | C5 |

Probe artifacts were temporary: `/tmp/cch-tui-audit-Xc4GbV/{probe.cpp,components.cpp,autocomplete.cpp,markdown.cpp,pi-probe.mjs}`. C++ probes compiled current UnicodeWidth/InputDecoder/TextBuffer/Fuzzy/Utils/Keys, Container/Text/Overlay, Autocomplete, and Markdown code, using pinned utf8proc/md4c; pi probes imported current TypeScript directly under Node v26. The observations recorded above are narrow executed results, not full toolkit acceptance.

No repository test suite, manual terminal session, or CLI/TUI E2E was run in this audit. No performance benchmark, image display, clipboard/native helper, or live terminal color negotiation was exercised. Source-derived expectations remain labeled accordingly. This deliverable changes documentation only; implementation, ADR changes, baseline advancement, and product acceptance remain subsequent work.
