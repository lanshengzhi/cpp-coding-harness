# fixtures/pi-mcp — MCP stdio evidence (spec #865)

Fixture inputs for the MCP-over-stdio slice (#869). The fixtures are Pike-side
test doubles, not pi transcripts: pi's own MCP transport is TypeScript and needs
a live Node/`bun` runtime and (for most servers) network, neither of which the
offline test suite uses. What is pinned instead is the **wire contract** the
transport must speak, held byte-for-byte where it is observable.

## `echo_server.py`

A real MCP stdio child process. Framing mirrors pi v1.0.4
`packages/mcp/src/transports/stdio.ts`: one compact JSON object per
`\n`-terminated line (no `Content-Length` framing), notifications on stdin,
responses on stdout, diagnostics on stderr.

| Method | Behavior |
| --- | --- |
| `initialize` | Returns `protocolVersion: 2025-06-18`, `capabilities`, `serverInfo`. |
| `notifications/initialized` | Notification; no response. |
| `tools/list` | One tool per page with `nextCursor`, exercising client pagination. |
| `tools/call` `echo` | Echoes `arguments.text` as a text content block. |
| `tools/call` `fail` | Returns a well-formed `isError: true` result. |
| `tools/call` `crash` | Exits without responding (connection closed mid-request). |
| `debug/emit_garbage` | Writes a non-JSON line, then the valid response. |
| `debug/emit_invalid_jsonrpc` | Writes valid JSON that is not a JSON-RPC message. |

`http_server.py` (the streamable-HTTP TLS fixture) also carries two debug
methods used by the OAuth slice (#875) to observe credential attachment over
the wire: `debug/authorization_present` returns the received `Authorization`
header (empty when none), and `debug/unauthorized` answers `401` with a
`WWW-Authenticate: Bearer` challenge. The MCP OAuth loopback exchange replay is
committed separately under `oauth/` (`oauth/README.md`).

The server offers three tools (`echo`, `fail`, `crash`) in the order `tools/list`
reports them.

## `golden/`

Committed expected observations compared structurally against a real run.

- `golden/discovered-tools.json` — the descriptors `connect_stdio` produces for
  `echo_server.py`: each server tool's `mcp__<server>__<tool>` name, description,
  and input schema, in server order. Pins the discovery + naming contract.
- `golden/tool-call-render.json` — the composed tool-execution block (fallback
  renderer, viewport 80) for a call to `mcp__echo__echo` with
  `{"text":"hello"}`. Pins that MCP tools render through the existing fallback
  pair exactly like pi's MCP tools, with no named renderer.

## Provenance / scope

- No live credentials, no network: the only external process is `python3`
  running `echo_server.py` locally.
- The goldens are captured from Pike's own product run against this server; they
  are a regression pin, not a pi byte-for-byte transcript.
