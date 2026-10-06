# codemode declarations — project-local script tools (#870)

This directory holds Pike's codemode declaration fixtures and documents the
declaration format. Sibling directories:

- `declarations/` — a valid project source set (`*.json` + their `*.js`).
- `invalid/` — one file per invalid-declaration error class, loaded
  individually by the tests in `tests/coding_agent/CodemodeDeclarationTest.cpp`.
- `duplicate/` — two valid declarations that declare the same tool `name`.
- `probe-wasm/` — the #868 wasm-runtime feasibility harness (unrelated to this
  format).

## Format

A project declares script tools by putting one JSON file per tool in
`<workspace>/.pi/codemode/`:

```
<workspace>/.pi/codemode/
  summarize_repo.json
  summarize_repo.js
```

Each `*.json` file is one declaration. Non-`.json` files in the directory are
ignored (that is where the `*.js` sources live). The fields are:

| field | required | meaning |
|---|---|---|
| `name` | yes | the tool name as the model sees it, used verbatim. Must be non-empty and unique across all tools. |
| `description` | yes | the tool description the model sees. |
| `inputSchema` | no | a JSON Schema **object** for the tool's single argument. Omitted means an empty object schema. |
| `source` | yes | path to a JavaScript file, resolved relative to the declaration file. |

The `*.js` source uses pi's codemode source format
(`packages/codemode/src/source.ts` at `7c10bd43`): JavaScript with an optional
first line `// @options: {...}`. Supported `@options` fields are
`max_output_tokens` (a non-negative integer) and `timeout_ms` (a positive
integer up to 2147483647). The options line is replaced by an empty line so
line numbers in stack traces stay aligned.

```js
// @options: {"max_output_tokens": 2000, "timeout_ms": 30000}
const issues = [{ id: 1 }, { id: 2 }, { id: 3 }];
return issues.length;
```

A declared script runs self-contained inside the sandbox. It cannot reach the
host tool surface: `tools.*` and globals are empty, so a call to one is an
explicit `does not exist` error rather than an unimplemented capability. Routing
a script's `tools.*` calls back to the session's tool set is a recorded
follow-up (codemode tool forwarding), not silently dropped — see the #874
notes.

## Errors are explicit

Loading is strict: a malformed declaration, a missing/empty `name`,
`description`, or `source`, an unknown field, a non-object `inputSchema`, an
unreadable source file, an empty source, a bad `@options` line, or a duplicate
tool name is a typed error that aborts the load. Nothing is skipped silently
and a partially loaded set is never returned.

## Presentation golden

`golden/tool-result-render.json` pins the rendered form of a declared tool's
result (#877): the `ToolRendererRegistry` fallback framing on screen (the bold
tool name, a blank row, the argument JSON, and the folded text output) plus the
inline image sidecar the component places after the tool block. A declared tool
name is unregistered, exactly like an MCP or extension tool name, so the
registry hands it the fallback pair and the image lands in the same inline slot
as any other tool's image; there is no codemode-specific renderer. The golden's
trailing `[Image: ...]` row is the image component's own placeholder, which the
terminal replaces with the sidecar when it supports inline images.

## Intentional divergence from pi v1.0.4

pi v1.0.4 has **no** on-disk codemode declaration format. In pi, codemode
scripts are written inline in the model's `codemode` tool call, and codemode
"tools" are the session's other registered tools (built-in, extension, or MCP)
selected by an in-memory `exposure` concept. There is no project-local
codemode source directory to mirror.

Pike defines this minimal project-local surface so a project can declare script
tools and see them on the tool surface. It follows pi where there is a shape to
follow: the source file format is pi's `source.ts`, and the model-facing
identity (`name`, `description`, `inputSchema`) is pi's `CodemodeTool`.

**Scope (#870, #874, #877):** declarations are loaded, validated, and listed. As of #874 a call to a
declared tool runs its script inside the wasm sandbox and returns the script's value and output; the
sandbox has no host filesystem, network, or module capability (see `quickjs/README.md`). As of #877
that output is presented through the existing `ToolRendererRegistry` fallback pair and the same
inline image slot as any other tool's output (see "Presentation golden" above). The script's
`tools.*` surface is empty in this slice, so a script that calls `tools.<name>` fails with an
explicit "does not exist" error. Routing `tools.*` to the session's tool set, and loading the
source into a session from settings/CLI, land in later slices of spec #865.
