# pi export-html template assets

Vendored from **pi** (`@earendil-works/pi-coding-agent`), frozen at revision
`71023aec1546d6d677a65669c5665c0927ce0dd0` (pi v1.3.0), path
`packages/coding-agent/src/core/export-html/`:

- `template.html` — the export document shell with the `{{CSS}}`,
  `{{SESSION_DATA}}`, `{{MARKED_JS}}`, `{{HIGHLIGHT_JS}}`, and `{{JS}}`
  substitution slots.
- `template.css` — the export stylesheet with the `{{THEME_VARS}}`,
  `{{BODY_BG}}`, `{{CONTAINER_BG}}`, and `{{INFO_BG}}` slots.
- `template.js` — the self-contained client renderer (session tree, markdown
  via marked, syntax highlighting via highlight.js, base64 `SESSION_DATA`
  decoding).
- `vendor/marked.min.js`, `vendor/highlight.min.js` — the minified libraries
  pi itself vendors into the template directory.

Files are copied **byte-for-byte** (no port, no reformat). The C++ HTML
exporter (`src/coding_agent/tui/HtmlExport.cpp`) performs pi's template
substitution and base64 session-data payload so the produced document matches
pi v1.3.0's export.

The files are installed with the Runtime under `share/pike/export-html/` so
an installed `pike` exports without the source tree; a source build resolves
this directory directly (see `HtmlExport.cpp`).

## License

pi is MIT-licensed; see [LICENSE](LICENSE) (Copyright 2025 Mario Zechner).
The installed Runtime ships this notice under `share/pike/licenses/`.
