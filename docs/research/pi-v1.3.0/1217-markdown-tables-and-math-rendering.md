# #1217 B9 Markdown, tables and math rendering recaptured at pi-v1.3.0

## Scope

Route batch B9 of 17 (`docs/research/pi-v1.3.0/1182-tui-route-batches.json`,
disposition `comparison`). Recapture the Markdown rendering (headings, lists,
tables, CJK wide characters, code blocks, blockquotes, links, hr, transforms,
backslash escapes, math integration, and default text styles) and the reusable
LaTeX math engine at the frozen pi-v1.3.0 baseline
(`71023aec1546d6d677a65669c5665c0927ce0dd0`) and compare the C++ rows through
the named callers `cch::tui::Markdown` (src/tui/Markdown.cpp) and
`cch::tui::render_latex` (src/tui/Latex.cpp). All 51 batch rows are receipted
whole-row through the delivery contract.

## Delivered behavior

- The `audit-1217` family captures the v1.3.0 behavior of the `Markdown` component
  and the `renderLatex` engine through the frozen checkout's own modules:
  1. `markdown-headings-and-inline-styles`: H1 and H2 headings, bold, italic,
     strikethrough, underline, and inline code formatting under theme styling;
  2. `markdown-lists-and-markers`: unordered bullet lists with nested indentation,
     sequential ordered list normalization (`1. `, `2. `, `3. `), and source
     marker preservation (`preserveOrderedListMarkers: true`);
  3. `markdown-tables-and-cjk`: standard GFM table borders and alignment, wide
     CJK character column measurement in terminal cells, and unequal column widths;
  4. `markdown-code-blocks-and-quotes`: fenced code blocks with custom indentation
     and border styling, syntax highlighter integration (`highlightCode`), and
     nested blockquotes;
  5. `markdown-links-hr-and-transforms`: hyperlink formatting with URL fallback,
     horizontal rules, available-width source transforms (`transform`), and
     backslash escape preservation (`preserveBackslashEscapes: true`);
  6. `markdown-latex-math-integration`: inline LaTeX (`$...$`), display LaTeX
     (`$$...$$`), disabled math preservation (`renderLatex: false`), and pending
     math fallback;
  7. `markdown-lifecycle-and-default-styles`: `DefaultTextStyle` pre-styled text
     decoration and foreground colors, default style prefix computation, text updates,
     and component cache invalidation;
  8. `inline-grammar`: full 41-case LaTeX inline grammar matrix (Greek symbols,
     operators, fractions, roots, accents, scripts, blackboards, and environments);
  9. `inline-failure-value`: malformed and unsupported LaTeX expression rejection
     returning no value;
  10. `display-option`: multiline baseline fraction, operator limits, and nested
      display math rendering.
- The 1050 toolkit capability ledger rows for all 51 B9 members now name these
  scenarios and their registered artifacts under `v1.3.0-observation`.
- C++ comparisons live in `tests/tui/MarkdownTest.cpp` and `tests/tui/LatexTest.cpp`
  tagged `[issue1217]`. Every claim carries an independent discriminating case
  satisfying the #1229 contract.

## Validation

- Focused comparison selection: `ctest --preset vcpkg -LE architecture -L issue1217`,
  29/29 passed (`deliveries/1217-issue1217.xml`).
- The 19 receipts that pin `baselines.json` or `consolidation-plan.json` were
  revalidated under `revalidation1217` after the `audit-1217` registration.
- Full Validation: `cmake --build --preset vcpkg`, `ctest --preset vcpkg -LE architecture`,
  `ctest --preset vcpkg -L architecture`.
- `python3 -m unittest tests.evidence.ConsolidationAuditTest` passed;
  `python3 scripts/evidence/receipt_boundary.py` exit 0.

## Capability status

All 51 batch rows advance to delivery-backed `verified` against the registered
audit-1217 family.

## Remaining boundary

None. All 51 rows in route batch B9 are receipted whole-row.
