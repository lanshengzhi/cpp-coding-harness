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
const issues = await tools.read({ path: "issues.json" });
return issues.length;
```

## Errors are explicit

Loading is strict: a malformed declaration, a missing/empty `name`,
`description`, or `source`, an unknown field, a non-object `inputSchema`, an
unreadable source file, an empty source, a bad `@options` line, or a duplicate
tool name is a typed error that aborts the load. Nothing is skipped silently
and a partially loaded set is never returned.

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

**Scope (#870):** declarations are loaded, validated, and listed. The script is
never executed — a call to a declared tool returns an explicit
"codemode execution is not wired until #874" error. Sandboxed execution, and
loading the source into a session by default (settings/CLI), land in later
slices of spec #865.
