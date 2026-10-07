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
| `tools/call` `hang` | Never responds; once the client sends `notifications/cancelled` for it, keeps streaming notification frames without ever completing the request. |
| `tools/call` `slow` | Sleeps `arguments.ms` (default 50 ms), then echoes `arguments.text`. |
| `notifications/cancelled` | Observed on the read loop; logged, and a `hang` request is answered with streaming notifications. |
| `debug/emit_garbage` | Writes a non-JSON line, then the valid response. |
| `debug/emit_invalid_jsonrpc` | Writes valid JSON that is not a JSON-RPC message. |
| `debug/exit` | Responds, then exits 0 (a server that dies between `tools/list` and `tools/call`). |


`http_server.py` (the streamable-HTTP TLS fixture) also carries two debug
methods used by the OAuth slice (#875) to observe credential attachment over
the wire: `debug/authorization_present` returns the received `Authorization`
header (empty when none), and `debug/unauthorized` answers `401` with a
`WWW-Authenticate: Bearer` challenge. The MCP OAuth loopback exchange replay is
committed separately under `oauth/` (`oauth/README.md`).

The server offers three tools (`echo`, `fail`, `crash`) in the order `tools/list`
reports them.

The server offers five tools (`echo`, `fail`, `crash`, `hang`, `slow`) in the
order `tools/list` reports them.

## Trace file

When `PIKE_MCP_TRACE` names a file, the server appends one flushed line per
observed event: `recv <method> id=<n>`, `hang-start id=<n>`, `stream id=<n>`,
and `cancelled requestId=<n>`. The failure-isolation tests set it through
`McpStdioServerConfig.env` and poll it to assert that a cancellation reached
the server, so a run that only failed the call locally cannot pass.

## `http_server.py`

A self-contained TLS MCP server for the streamable-http slice (#873), speaking
the same wire contract as `echo_server.py` over `POST`/SSE and issuing an
`Mcp-Session-Id` the client must echo. It offers `echo`, `fail`, `sse_echo`,
`hang`, `slow`, and `crash`; the last three are the failure-isolation modes
(#872): `hang` holds a request open until the client cancels it, `slow` sleeps
`arguments.ms` before answering, and `crash` exits the server without
responding. The same `PIKE_MCP_TRACE` trace file applies (`recv <method>
id=<n>`, `hang-start id=<n>`), so a test can prove a request was in flight
before it cancelled it.


## `mcp.json`

A sample global MCP server configuration in pi's `mcpServers` shape (ticket
#876): two stdio entries — one enabled, one `"enabled": false` — and one
streamable-http entry. The committed file is the input the persistence loader
must read; it is not launched. Its projection is pinned by
`golden/mcp-config-servers.json`.

## `golden/`

Committed expected observations compared structurally against a real run.

- `golden/discovered-tools.json` — the descriptors `connect_stdio` produces for
  `echo_server.py`: each server tool's `mcp__<server>__<tool>` name, description,
  and input schema, in server order. Pins the discovery + naming contract.
- `golden/tool-call-render.json` — the composed tool-execution block (fallback
  renderer, viewport 80) for a call to `mcp__echo__echo` with
  `{"text":"hello"}`. Pins that MCP tools render through the existing fallback
  pair exactly like pi's MCP tools, with no named renderer.
- `golden/mcp-config-servers.json` — the path-free projection (name, enabled,
  kind, command/args or url/headers) of the entries `mcp.json` loads into, in
  the loader's key order. Pins the `mcpServers` shape and the `enabled` flag.

## Provenance / scope

- No live credentials, no network: the only external process is `python3`
  running `echo_server.py` locally.
- The goldens are captured from Pike's own product run against this server; they
  are a regression pin, not a pi byte-for-byte transcript.
