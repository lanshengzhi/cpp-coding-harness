# Phase 1 inventory: cch_tui against pi TUI v1.0.0

## Scope and method

- Pike scope: src/tui/ and its tests, the cch_tui foundation lane. Product prompt/UI code in src/coding_agent/tui/ belongs to lane #7 and is not assessed here.
- Upstream: pi release v1.0.0, exact commit a13d35a742c6ef8462812a28fbe1d8c8b7431c32, package packages/tui.
- ADR 0035's reference 83114817c68f5413e4d7ba6d7003ddc511cd31d2 is not an ancestor of this upstream commit (merge base aa0ec808b970db31822e07835a46647cb51d9d66). ADR 0060's pin f07218c4d4bbc12bef056a7058c3dd49dfe41abe is an ancestor. Findings compare against exact v1.0.0; the f07218c4..a13d35a7 window is used only to identify later upstream changes.
- This is an inventory, not a product decision or implementation proposal. Pi code is comparison evidence, not Pike design authority (ADR 0053).
- Pike has 69 tracked files under src/tui/ (19,199 lines); pi's package tree has a different structure and includes tests/native/docs, so raw file or LOC totals are not treated as capability parity measures.

## Findings

### 1. Autocomplete after opening wrappers — Partial

Pi's packages/tui/src/autocomplete.ts adds stripLeadingWrappers and applies it to attachment/path prefixes. packages/tui/test/autocomplete.test.ts covers @ after wrappers and paths after (, [, {, <, and backticks.

Pike's extract_at_prefix and extract_path_prefix in src/tui/Autocomplete.cpp derive a prefix after the last delimiter without stripping unmatched opening wrappers. Editor.cpp::autocomplete_pattern_matches only begins a token after spaces/tabs, so a preceding wrapper also prevents the @ trigger from matching. Ordinary autocomplete works, but these wrapped forms are not represented.

- Rationale: no intentional-divergence rationale found in the reviewed ADRs.
- Validation: add focused cases to tests/tui/AutocompleteTest.cpp and exercise through EditorTest.cpp; run ctest --preset vcpkg -LE architecture -L tui.
- Dependency: editor trigger detection and provider prefix extraction must agree on the preserved completion prefix.

### 2. Slash-command completion with leading whitespace — Partial

Pi autocomplete.ts trims leading whitespace for matching while preserving it in the replacement prefix. autocomplete-skill-slash.test.ts covers leading whitespace and skill-name shorthand.

Pike's editor context check trims leading whitespace, but CombinedAutocompleteProvider::get_suggestions gates slash matching with text_before_cursor.starts_with('/') in src/tui/Autocomplete.cpp. The provider path therefore does not mirror pi's tested " /" and "  /mod" behavior. Pike already supports ordinary slash commands; this is a narrower trigger/prefix difference. Skill shorthand matching should be compared separately before implementation because Pike's command registry may expose skill names differently.

- Rationale: no intentional-divergence rationale found.
- Validation: add leading-space cases to tests/tui/AutocompleteTest.cpp and preserve whitespace on apply; run ctest --preset vcpkg -LE architecture -L tui.
- Dependency: confirm the provider receives the untrimmed editor prefix and agree on replacement-prefix handling.

### 3. ANSI ordering at a slice_by_column start boundary — Partial

Pi v1.0.0 added test/regression-slice-by-column-ansi-order.test.ts. One case expects sliceByColumn("\x1b[32mfoo\x1b[39m bar", 3, 4, true) to return "\x1b[32m\x1b[39m bar".

In Pike src/tui/Utils.cpp::slice_by_column, ANSI before the slice is queued in pending_ansi, but an ANSI token exactly at start_col is appended immediately; the queued style is flushed only upon the next included grapheme. By source-order inspection this yields reset-before-green for the cited case, unlike pi's expected style-then-reset order. Current tests/tui/UtilsTest.cpp checks carried leading style but not this reset-at-start case.

- Rationale: no intentional-divergence rationale found.
- Validation: add the upstream regression case to tests/tui/UtilsTest.cpp; run ctest --preset vcpkg -LE architecture -L tui.
- Dependency: none known for Phase 2.

### 4. Truecolor detection for TERM values ending in -direct — Partial

Pi packages/tui/src/terminal-image.ts::detectCapabilitiesFromEnvironment treats term.endsWith("-direct") as a truecolor hint, including tmux/screen paths.

Pike src/tui/ProcessTerminal.cpp::detect_color_capability checks COLORTERM and known emulator variables/terminal names but has no -direct check. With TERM=<name>-direct and no other color hint, Pike reports xterm-256 while pi reports truecolor.

- Rationale: no intentional-divergence rationale found.
- Validation: add an isolated environment case to tests/tui/ProcessTerminalTest.cpp; run ctest --preset vcpkg -LE architecture -L tui.
- Dependency: none known for Phase 2.

### 5. Kitty image aspect-ratio sizing — Partial

Pi terminal-image.ts::renderImage enables calculateImageCellSize(..., optimizeAspectRatio=true) for Kitty; chooseLessDistortedCellCount adjusts a rounded cell dimension to reduce distortion.

Pike src/tui/OverlayCompositor.cpp computes image rows/columns by ceiling the scaled pixel dimensions. It does not apply the nearest-cell distortion adjustment, including for Kitty candidates. Existing image composition and Kitty rendering are present; this is a sizing difference.

- Rationale: no intentional-divergence rationale found.
- Validation: add a Kitty image sizing case to tests/tui/OverlayCompositorTest.cpp or TerminalImageTest.cpp; run ctest --preset vcpkg -LE architecture -L tui.
- Dependency: none known for Phase 2.

## Out of scope or already classified

- **Explicitly Deferred by ADR 0035:** pi's alt-screen/viewport stack (TuiAltScreen, layout engine, ScrollView, stacks, mouse/wheel/selection/scrollbars, OSC 133 navigation), extension-only EditorComponent and input listeners, live color-scheme notifications and TUI-level background/scheme queries, diagnostic-only exports, and unconsumed parser/public APIs. These are not implementation gaps for the currently scoped regular TUI paths. Startup OSC 11/?997 detection remains Supported.
- **Transferred to lane #7:** pi's colors.ts / oklab.ts APIs are consumed by product theme/UI code; the Pike counterpart belongs to src/coding_agent/tui/Theme.*, not src/tui/. Record this as a cross-lane ownership item rather than a cch_tui gap. Full terminal-color querying remains explicitly Deferred in ADR 0035.
- **No decision recorded here:** any new capability not covered by reviewed Pike decisions remains an open product question. This report does not infer intentional exclusion from absence in Pike or in the prompt.
- Pi-only performance changes in components/rendering were not listed as gaps absent evidence of a user-visible behavior change.

## Validation and dependency notes

No tests or builds were run; this Phase 1 deliverable is source and decision inventory only. Suggested Pike regression suite for any later TUI change:

    ctest --preset vcpkg -LE architecture -L tui

The final architecture gate, if a later implementation requires it:

    ctest --preset vcpkg -L architecture

Phase 1 has no dependencies. The report does not assert sequencing among future Phase 2 work; the editor/provider coupling is called out only where the behavior crosses those existing seams.
