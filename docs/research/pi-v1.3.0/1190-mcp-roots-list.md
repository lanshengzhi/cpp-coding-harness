# #1190 — MCP connection roots and server-request handlers

## Scope

Delivers the `cch_mcp:mcp-roots-list` row's C++ surface: the roots the client
answers `roots/list` with ride the connection (pi `McpClientOptions.roots`), and
the same `ping` / `roots/list` handler set is installed for the stdio and
streamable-HTTP transports. A server request with no handler is answered
`-32601` on both. The slice also adds one frozen pi-v1.3.0 observation per
transport and one shared no-handler observation, then compares the C++ answers
to that capture.

Authority: [ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md),
[#1181](https://github.com/lanshengzhi/cpp-coding-harness/issues/1181) and the
[#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044) delivery,
with [#1190](https://github.com/lanshengzhi/cpp-coding-harness/issues/1190) as
its ticket. Named baseline: `pi-v1.3.0` /
`71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc.

Registering the capture family, routing its scenarios and issuing the whole-row
delivery receipt stay with the serial merger. This record makes no capability
promotion: the row keeps `partial` / `unverified` in the consolidated ledger.

## Frozen contract and capture

Source of truth at the pinned revision:

- `packages/mcp/src/client.ts` — `McpClient` installs `ping` unconditionally and
  `roots/list` only when `McpClientOptions.roots` is set; `connect` advertises
  the `roots` client capability only for a connection that carries roots;
  `handleRequest` answers an unknown method with code `-32601` and message
  `Method not found: <method>`, and a handler failure with `-32603`. The
  configured roots are returned verbatim (`{roots: [...]}`, `name` omitted when
  the connection did not set one).
- `packages/mcp/src/transports/stdio.ts` and
  `packages/mcp/src/transports/streamable-http.ts` — one request/response shape
  over both wires; the HTTP transport also opens the server-to-client GET stream
  after `notifications/initialized`.
- `packages/coding-agent/src/extensions/mcp/runtime.ts` — the application builds
  `roots: [{uri: pathToFileURL(cwd).href, name: basename(cwd)}]` from its
  session `cwd` and passes it to `McpClient`. That application wiring is
  explicitly outside this slice (see Remaining boundary).

The independent capture lives in `fixtures/pi-v1.3.0/audit-1190/`. Its script
runs the real frozen `McpClient` over a real `StdioTransport` child
(`fixtures/pi-mcp/echo_server.py`) and over `StreamableHttpTransport`
(`fixtures/pi-mcp/http_server.py`), both in a new `roots` mode. The mode issues
`ping` (id 101), `roots/list` (id 102) and an unhandled
`sampling/createMessage` (id 103) after initialization and records the client's
response bytes verbatim. The capture refuses a checkout that is not the
registered revision or is dirty, and fails unless the recorded answers match
the scenario names, so the expectation cannot be synthesized from C++.

| Artifact scenario | Observation |
| --- | --- |
| `a stdio server ping is answered with an empty result and roots/list returns the connection's configured roots` | `{result:{}}` for ping; `{result:{roots:[{uri:"file:///workspace",name:"workspace"}]}}` for roots/list |
| `a streamable HTTP server ping is answered with an empty result and roots/list returns the connection's configured roots` | the same two answers over the server-to-client GET stream |
| `a server request with no matching handler is answered -32601 before the connection closes` | both transports answer `{error:{code:-32601,message:"Method not found: sampling/createMessage"}}` |

The manifest is a validated candidate (`scripts/evidence/audit_evidence.py check
--manifest fixtures/pi-v1.3.0/audit-1190/manifest.json` passes; manifest SHA-256
`5413812fd6eb0b1e26aac5e2c4594572ac0c14c7377d7b4aca82b84e927213ec`). It is not
registered in `baselines.json`; the serial merger applies the printed registry
record.

## Delivered behavior

- `cch::mcp::Root` and `roots` on both connection descriptors
  (`HttpServerConfig`, `StdioServerConfig`), documented as pi
  `McpClientOptions.roots`. An engaged value (even empty) installs the handler
  and advertises the capability; a disengaged value installs neither, so a
  server's `roots/list` is answered `-32601`. The client never derives a root
  from the process working directory; the previous `file://<cwd>` handler is
  gone.
- `Client` owns the server-request surface: `ServerRequestHandler` and
  `set_request_handler` moved from `HttpClient` to the transport-independent
  contract. `Client.cpp` installs the one default set both transports use
  (`ping` always; `roots/list` when the connection carries roots), and the
  response builders are shared in the private protocol header.
- The stdio pump reads continuously and serves an incoming server request
  before matching its own response, writing the answer on the serialized pump:
  a registered handler answers its result (a handler error becomes `-32603`),
  an unregistered method answers `-32601`, and an idle connection now reads
  even when no notification listener is installed. The HTTP GET-stream path
  keeps its existing dispatch and uses the shared response shape.
- `initialize_params(with_roots)` follows the connection: the `roots`
  capability is advertised only when roots are configured, matching the frozen
  `connect` behavior.

## Comparison cases

| Frozen scenario | C++ comparison case (`tests/mcp/McpOwnerTest.cpp`) |
| --- | --- |
| stdio ping + roots/list | `cch_mcp stdio client answers a fixture server's ping and roots from the connection` |
| streamable HTTP ping + roots/list | `cch_mcp streamable HTTP client answers a fixture server's ping and roots from the connection` |
| no-handler `-32601` on both transports | `cch_mcp clients answer a fixture server's unhandled request -32601 on both transports` |

The `[frozen]` cases load the named scenario's `expected` members from
`fixtures/pi-v1.3.0/audit-1190/server-requests.json` and structurally compare
the fixture-recorded response bytes, so the expectation is the artifact's own
observation. Each case closes its client before the discriminating assertions
(release-then-assert).

Discriminating cases that fail on the recorded alternatives:

- `MCP GET stream: server request roots/list is answered with the connection's
  configured roots` (`tests/coding_agent/McpHttpGetStreamTest.cpp`) records the
  exact configured roots; the retired `file://<cwd>` answer fails it.
- `MCP GET stream: server request roots/list without connection roots is
  answered -32601` (`[discriminating]`) fails if a connection without roots
  still answers from the process cwd.
- `initialize omits the roots capability when the connection carries no roots`
  fails if the capability is advertised unconditionally.
- The stdio cases fail if a server request is left unanswered (no recorded
  response) or if only one transport installs the handler set.

## Validation

Measured on the merged tree, checkout base
`50a45c53af89a0344638b666037bba517a05de4d` (`origin/main` merged into `1190`):

- Focused: the new comparison selection passed 9/9; the MCP-related selection
  (`ctest --preset vcpkg -R 'MCP|mcp' -j2`) passed 166/166.
- Full build: `cmake --build --preset vcpkg -j4` passed.
- Architecture: 43/43 selected names passed (`ctest --preset vcpkg -L architecture`).
- Full suite: serial `ctest --preset vcpkg` selected 4,217 and reported 7
  failures, none in MCP or consolidation. Six are load-sensitive cases that
  passed when rerun individually (`Durable edit tool … BOM CRLF`,
  `Process Terminal atomic stop … EIO`, `a timed-out query …`,
  `Native TUI InteractiveEngine … cursor coordinates`,
  `Native TUI engine ProcessTerminal PTY exit …`,
  `E2E: a focused-editor submission …`). The seventh,
  `real PTY records Herdr markdown and OSC 52 terminal bytes`, requires
  `TERM_PROGRAM=herdr` and this host reports `ghostty`; it is an environment
  capture, not a code path this slice changes. A parallel run additionally hit
  the already quarantined `Projection … issue cost bound` case (#632).
- Delivery revalidations on the merged tree (fresh JUnit artifacts, changed
  input digests refreshed, `revalidation1190` entries recorded):
  `1101-structured-content` 36/36 into
  `deliveries/1190-revalidation-1101.xml`; `1118-codemode-catalog` 19/19;
  `1181-mcp-comparisons` 16/16; `1189-close-refresh` 9/9.
- `consolidate_audits.py build` and `check` passed: 1,464 source rows, 45 rows
  backed by pinned comparison results, 614 scenarios, 196 explicit orphans, 0
  uncovered. `tests/evidence/ConsolidationAuditTest.py`: 42 passed.
- `scripts/evidence/audit_evidence.py verify`: 52 registered bundles, 108
  artifacts, 617 scenarios at the frozen revision; the audit-1190 candidate
  validates through `check` but is not registered.
- `scripts/format-check.sh origin/main` and `git diff --check`: clean.

## Capability status

`cch_mcp:mcp-roots-list` keeps `partial` / `unverified` in
`1058-consolidated-capability-ledger.json`. This slice supplies the
implementation, the independent capture and the comparison cases; the row
advances only when the serial merger registers `audit-1190` in
`baselines.json`, links the three scenarios to the row in the consolidation
plan, and records the whole-row delivery receipt from these cases.

## Remaining boundary

- Registration, scenario routing and the delivery receipt belong to the serial
  merger; no capability row changed state here.
- The application does not yet inject its session-workspace root at the
  `McpLiveConnection` adapter seam the way frozen `runtime.ts` does
  (`roots: [{uri: pathToFileURL(cwd).href, name: basename(cwd)}]`). The client
  therefore advertises roots only for a connection configured with them. That
  consumer wiring is application management/binding work under
  [#1177](https://github.com/lanshengzhi/cpp-coding-harness/issues/1177); it is
  named here so the client seam is not read as an application behavior claim.
- The stdio pump serves one server request at a time, so a long-running custom
  handler delays the pending client response; the default `ping` and
  `roots/list` handlers are immediate. Cancellation of an in-flight
  server-request handler (`notifications/cancelled`) remains an HTTP-only
  behavior.
- [#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044) remains
  open; closing this slice does not accept its remaining obligations.
