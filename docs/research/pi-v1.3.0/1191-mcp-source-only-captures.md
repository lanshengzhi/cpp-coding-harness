# #1191 MCP source-only protocol captures

## Scope

Freeze the missing pi-v1.3.0 observations for five supported `cch_mcp` rows and compare the
delivered C++ public seams against them, under [#1181](https://github.com/lanshengzhi/cpp-coding-harness/issues/1181),
[#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044) and
[#1183](https://github.com/lanshengzhi/cpp-coding-harness/issues/1183). Authority: [ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md),
Named Baseline **pi-v1.3.0** at `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with
glibc.

| Row | Frozen artifact | Scenarios |
| --- | --- | ---: |
| `cch_mcp:mcp-tools-list` | `audit-1191/mcp-tools-list.json` | 7 |
| `cch_mcp:mcp-progress` | `audit-1191/mcp-progress.json` | 5 |
| `cch_mcp:mcp-request-cancellation` | `audit-1191/mcp-request-cancellation.json` | 7 |
| `cch_mcp:mcp-http-session-stream` | `audit-1191/mcp-http-session-stream.json` | 7 |
| `cch_mcp:mcp-stdio-stderr-tail` | `audit-1191/mcp-stdio-stderr-tail.json` | 2 |

This ticket captures observations only. It changes no `src/mcp`, `src/coding_agent` or `src/durable`
file and adds no C++ assertion. A capture that exposes an unmatched field is reported below; the row
is not promoted through this record alone.

## Frozen evidence

The new family is [`fixtures/pi-v1.3.0/audit-1191/`](../../../fixtures/pi-v1.3.0/audit-1191/):

| File | Role |
| --- | --- |
| `capture-1191.mts` | executable capture, revision/clean/host guards and manifest writer |
| `scenarios.mts` | the 28 scenario functions; each returns one observation |
| `fixtures.mts` | loopback HTTP fixture and real-stdio fixture lifecycle |
| `stdio-server.mjs` | real child: newline-delimited MCP server with protocol, progress, cancellation and stderr modes |
| `pi.mts` | imports the frozen checkout named by `PI_CHECKOUT` |
| `manifest.json` | pins the generator, every helper source, the environment and the artifact bytes |

The capture resolves the baseline by name from
[`baselines.json`](../../../fixtures/pi-v1.3.0/baselines.json), refuses a checkout whose `HEAD` is
not `71023aec1546d6d677a65669c5665c0927ce0dd0` or whose tracked tree is dirty, refuses a non-Linux
x64/glibc host, and refuses a family already registered. It re-executes itself under the checkout's
`tsx` with `--conditions=source`, so the recorded runtime belongs to the frozen install. The
observations are produced by the frozen pi modules, not by C++ and not by a reimplementation.

`audit_evidence.py check` printed the candidate registry record:

```json
{
  "audit-1191": {
    "manifest": "audit-1191/manifest.json",
    "manifestSha256": "2a0df73764236ccac9909bd2b6202b7ae7b75da0d5214877b9f4a99f4ae15a95"
  }
}
```

Registering the family and adding its `orphanScenarios` entries is the serial merger's step; this
lane did not edit `baselines.json` or the consolidation plan.

## Observations

### `mcp-tools-list` — pagination and application naming

`McpClient.listTools` was driven through a real loopback HTTP fixture that returns pages on demand.

- Two pages with `nextCursor: "page-2"` then `""` produce `["search", "read"]`; the recorded cursors
  are `[null, "page-2"]` and the walk stops on the empty terminal cursor.
- `nextCursor: null` and `nextCursor: ""` each end after one page. Both are treated as absent, not
  echoed into another request.
- A server that repeats `"loop"` fails with `MCP tools/list returned duplicate cursor: loop` after
  two requests; a server that always returns a fresh cursor fails with
  `MCP tools/list exceeded 1000 pages` after exactly 1000 requests.
- `createMcpToolName` keeps a 64-character sanitized name plain, shortens a longer name to 64
  characters with `_<8-hex-sha256>` over the **raw** `${server}\0${tool}` pair, and hashes a taken
  plain name. The captured values are
  `mcp__my_server__get_item_v2`, a 64-character boundary name, `mcp__srv__a_b_ed3867d6`, and
  `mcp__server__…_9fc39f5e`.
- The application extension was driven with two tools that sanitize to `mcp__srv__a_b` in both list
  orders. Both tools take the hash suffix and the raw-tool assignment is identical in both orders
  (`mcp__srv__a_b_3d325570` for `a-b`, `mcp__srv__a_b_5c15ffbd` for `a_b`).

### `mcp-progress` — token, delivery, text and re-arming

- Over stdio and HTTP, `callTool(..., {onProgress})` sends `_meta.progressToken: 2` (the JSON-RPC
  request id). Only `notifications/progress` with that token is delivered; a token `+1000` and a
  notification after settlement are ignored. Both transports deliver the same two events:
  `{progress: 1, total: 2, message: "halfway"}` and `{progress: 2}`.
- The application text callback uses `message` verbatim when present, otherwise
  `Progress <progress>[/<total>]`: `"custom message"`, `"Progress 2/10"`, `"Progress 3"`, and `""`
  for an empty message.
- With `timeoutMs: 70`, a progress notification at 40 ms and another at 80 ms re-arm the deadline, so
  the result at 110 ms resolves instead of timing out. The captured elapsed time is 111 ms over
  stdio and 112 ms over HTTP (both `>= 80ms`), with two progress events.

### `mcp-request-cancellation` — abort, timeout and no-notify cases

- Aborting a held stdio `tools/call` with `AbortController.abort("stop")` settles with `AbortError`,
  sends one `notifications/cancelled` for request id 2 with reason `"stop"`, and the same child then
  serves `echo:after`.
- Aborting a held HTTP call does the same over the loopback fixture: one cancellation POST for
  request id 2 with reason `"stop"`, and the later call carries `mcp-session-id: session-1`.
- With `timeoutMs: 80`, the caller settles with `McpTimeoutError` (`MCP request timed out after
  80ms`) and the server receives `notifications/cancelled` for request id 2 with reason
  `"Request timed out"` over both transports.
- An already-aborted signal sends no `tools/call` and no cancellation. A failed HTTP call (status
  500) and a failed stdio call (JSON-RPC `-32000`) emit no cancellation; aborting after the stdio
  call settled also emits none.

### `mcp-http-session-stream` — session headers and the GET stream

- The initialize response's `mcp-session-id: session-1` is captured by the transport; `tools/list`
  carries `mcp-session-id: session-1` and `MCP-Protocol-Version: 2025-06-18` (the negotiated
  version), the GET carries `Accept: text/event-stream` and the same session id, and `close()` sends
  `DELETE` with the session id.
- A `405` GET is feature-absent: one GET, no retry, no error, and `tools/list` still works.
- A `retry: 20` field opens the second GET after 23 ms with `Last-Event-ID: 1`, overriding the
  configured 500 ms client backoff.
- A retry field of `99999999999999999999999999` is accepted by pi as `1e+26` and the runtime clamps
  its timer to the 1 ms floor; the second GET arrives after 6 ms with `Last-Event-ID: 1` and no
  error. This is Node's timer clamp, not a pi-level rejection (see the comparison note below).
- A live GET delivers `notifications/message` to `onNotification`, answers server `ping` with `{}`
  and `roots/list` with the configured `[{uri: "file:///workspace", name: "workspace"}]`, and a
  `notifications/cancelled` aborts an in-flight `test/wait` handler (the handler observes the abort
  and the client posts `{code: -32603, message: "MCP request aborted"}`).
- `close()` aborts the held GET (`get-close` observed), sends the session `DELETE`, and emits no
  dropped-stream error.

### `mcp-stdio-stderr-tail` — the ring and the failure tail

- A real child writes 80,000 bytes to stderr and exits before answering initialize.
  `StdioTransport.stderr` is exactly 65,536 bytes, ends with `-AUDIT1191-END`, and does not contain
  the `AUDIT1191-START-` head: the ring keeps the last 64 KiB.
- The application `McpServerConnection` fails the connect with
  `MCP server "audit-1191-stderr" failed to connect: MCP connection closed` followed by the last
  2,000 characters of the trimmed ring. The prefix, the exact tail and the absence of the ring head
  are recorded; the error is not the whole 80,000-byte stream.

## Comparison with the delivered C++ seams

The comparison uses the captured scenario names as the unit. `tests/coding_agent` cases are from
the `-L mcp` run recorded under `## Validation`.

| Row | Captured scenario | Delivered C++ evidence | Result |
| --- | --- | --- | --- |
| `mcp-tools-list` | follows `nextCursor` across pages, ends on `""` | `tools/list pagination follows a nextCursor across pages` | matched |
| `mcp-tools-list` | `null` and `""` terminal cursors | `tools/list treats a null or empty nextCursor as the end of pagination` | matched |
| `mcp-tools-list` | duplicate cursor error | `tools/list rejects a duplicate cursor with pi's message` | matched (same message) |
| `mcp-tools-list` | 1000-page bound | `tools/list stops after pi's 1000-page bound` | matched (same message, 1000 pages) |
| `mcp-tools-list` | sanitization, 64-character bound, raw-pair hash | `tool names within pi's length bound keep the sanitized form`, `a tool name over pi's 64-char bound takes the sha256 suffix from the bundle example`, `a taken tool name takes the sha256 suffix from the bundle collision example` | matched by the same `sha256(${server}\0${tool})` formula; the captured exact examples are new |
| `mcp-tools-list` | order-independent collision assignment | `colliding tool names are assigned order-independently, every colliding tool hashed` | matched; the captured exact names are new |
| `mcp-progress` | `_meta.progressToken` injection and matching delivery | `request with on_progress injects _meta.progressToken and progress_token_of extracts it` plus `McpClient.requestInternal`/`handleProgress` | matched for token identity; delivery itself has no direct real-transport C++ case |
| `mcp-progress` | progress text | `progress_update_text formats pi progress messages verbatim` | matched |
| `mcp-progress` | deadline re-arm | `McpClient.armTimeout`/`handleProgress` and `HttpClient`/`StdioClient` read loops (source-reviewed) | matched by source; no direct C++ case |
| `mcp-request-cancellation` | stdio abort, notification and reuse | `a cancelled in-flight MCP call notifies the server, fails the call, and leaves the connection usable` | presence, request id and reuse matched; **reason differs** |
| `mcp-request-cancellation` | HTTP abort, notification and reuse | `cancelling an in-flight MCP HTTP call surfaces the cancellation error and keeps the connection usable`, `cancelling an in-flight MCP HTTP call tells the server with notifications/cancelled` | presence and request id matched; **reason differs** |
| `mcp-request-cancellation` | timeout error and reason | `a timed-out MCP HTTP call tells the server with the pi timeout reason`, `a hung MCP server fails the call with an explicit timeout, never a silent hang` | HTTP matched; **stdio reason differs** |
| `mcp-request-cancellation` | no notification for already-cancelled/failed | `a request already cancelled before it is sent does not emit notifications/cancelled`, `a failed MCP HTTP call does not emit notifications/cancelled`; stdio failure source-reviewed | matched |
| `mcp-http-session-stream` | session headers, 405, DELETE | `MCP GET stream: HTTP 405 is feature-absent, not a failure` plus `StreamableHttpTransport.buildHeaders` | matched; the GET session header is asserted, the POST headers are source-reviewed |
| `mcp-http-session-stream` | retry hint and `Last-Event-ID` | `MCP GET stream: a server retry field overrides the reconnect delay` | matched |
| `mcp-http-session-stream` | out-of-range hint | `MCP GET stream: an out-of-range server retry field is ignored, never throws` | both bounded and reconnect; exact delay differs (see note) |
| `mcp-http-session-stream` | event delivery | `MCP GET stream: server notifications reach the connection listener` | matched |
| `mcp-http-session-stream` | ping and roots answers | `MCP GET stream: server ping is answered with an empty result`, `MCP GET stream: server request roots/list is answered with file://<cwd>` | ping matched; roots shape matched, root value differs (B2) |
| `mcp-http-session-stream` | cancellation of a server handler | `MCP GET stream: notifications/cancelled aborts the in-flight handler` | matched (`-32603`, `MCP request aborted`) |
| `mcp-http-session-stream` | close without a dropped-stream error | `MCP GET stream: close() aborts the stream without the dropped error` | matched |
| `mcp-stdio-stderr-tail` | 64 KiB ring | `StdioTransport.handleStderr`/`kStderrBufferBytes` (source-reviewed); `an MCP process failure includes a bounded stderr tail` | matched |
| `mcp-stdio-stderr-tail` | 2000-character failure tail | `an MCP process failure includes a bounded stderr tail`; `McpServerConnection.connectOnce`/`connectFailed` | matched bound and content; C++ frames the tail with `stderrTail: ` |

### Unmatched fields

1. **Stdio cancellation reason is not pi's.** The capture recorded `reason: "stop"` for the abort and
   `reason: "Request timed out"` for the timeout. `src/mcp/StdioClient.cpp` `write_cancellation`
   hard-codes `{"reason": "cancelled"}` for both paths. The delivered stdio test only checks
   `cancelled requestId=`, so it does not separate this field. This is a behavior gap in
   `cch_mcp:mcp-request-cancellation`; the row stays partial until the stdio notification reason is
   matched (the C++ `RequestOptions` currently has no reason channel).
2. **HTTP abort reason is a fixed fallback, not the caller's reason.** The capture used
   `AbortController.abort("stop")`, so pi sends `"stop"`. `HttpClient.cpp` sends `"Aborted"` for a
   cancellation and `"Request timed out"` for a timeout. The timeout field matches; the abort field
   is the reviewed seam where C++ exposes a `std::stop_token` without a reason payload. This record
   does not claim the abort reason as a matched field.
3. **Out-of-range retry is bounded, by different mechanisms.** pi accepts the digits-only field as
   `1e+26`; Node clamps the timer to 1 ms, so the capture reconnects after 6 ms. C++ rejects the
   value as outside its integer range and falls back to the 20 ms client backoff. Both are bounded,
   never throw and resume with `Last-Event-ID: 1`; the exact delay is recorded as a normalized
   platform value, not as a pi-level rejection.
4. **Roots value.** The capture configures `file:///workspace`; the C++ connection composes
   `file://<cwd>`. The answer shape and the `roots/list` handling match. The exact root selection
   belongs to `cch_mcp:mcp-roots-list` (B2, #1190), not to this row.

### Reviewed seams without a direct C++ case

- Progress delivery and deadline re-arming over a real stdio child and a real HTTP SSE response are
  implemented in `McpClient.requestInternal`/`handleProgress` and the transport read loops. The
  existing `McpProgressTest` cases pin token construction and text formatting; they do not drive a
  real transport. The capture is the frozen observation, and the source paths above are the
  comparison for the transport half. This is recorded as a reviewed seam, not as a JUnit-backed
  field.
- The exact v1.3.0 tool-name examples (`mcp__srv__a_b_3d325570`,
  `mcp__srv__a_b_5c15ffbd`, `…_9fc39f5e`) are new observations. `McpToolNameTest` pins the same
  algorithm with the older bundle examples and the order-independence case; the exact values follow
  from the captured raw pair and the C++ `tool_name_hash_suffix` formula.
- The stdio failure tail's `stderrTail: ` label is C++'s error framing; the bound and content match
  pi's application tail.

## Validation

Measured in this worktree at base revision `b075f866219152078ab024eaf76aaba547d1bd5e` (the
capture was produced from the frozen checkout at `71023aec…`):

| Command | Result |
| --- | --- |
| `PI_CHECKOUT=/tmp/cch-1080-pi-frozen PI_BASELINE=pi-v1.3.0 /tmp/cch-1080-pi-frozen/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1191/capture-1191.mts` | 5 artifacts, 28 scenarios; manifest SHA-256 `2a0df73764236ccac9909bd2b6202b7ae7b75da0d5214877b9f4a99f4ae15a95` |
| `python3 scripts/evidence/audit_evidence.py check --manifest fixtures/pi-v1.3.0/audit-1191/manifest.json` | PASS, candidate record printed |
| `cmake --preset vcpkg` | configured; build-phase Parity Architecture Gate PASS (12 targets, 66 dependencies, 284 sources, 3,907 includes) |
| `cmake --build --preset vcpkg` | all targets built |
| `ctest --preset vcpkg -L mcp --output-junit docs/research/pi-v1.3.0/deliveries/1191-mcp.xml` | 304 selected, 304 passed, 0 failed, 0 skipped |
| `ctest --preset vcpkg -L architecture --output-junit docs/research/pi-v1.3.0/deliveries/1191-architecture.xml` | 43 selected, 43 passed, 0 failed, 0 skipped |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py build` | rebuilt; 1,464 rows, no diff against the committed ledger |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check` | PASS — 1,464 rows; 614 scenarios; 196 explicit orphans; 0 uncovered |
| `scripts/format-check.sh origin/main` | clean (fixture/docs only; no C++ formatting changes) |

The `-L mcp` count is the full-set build count; the earlier shard-only build reports 232, because
`catch_discover_tests` registers only the targets that were built. The full preset was built before
the label run. The `-L architecture` count is the expected 43 and all pass.

## Capability status

The five rows gain a registered-candidate frozen family and a named comparison:

- `mcp-tools-list` and `mcp-http-session-stream` have a complete captured field set matched by the
  existing C++ cases (with the out-of-range delay normalized and the roots value routed to B2).
- `mcp-progress` and `mcp-stdio-stderr-tail` have complete captures; their transport/framing halves
  are matched through the named source seams, while their existing C++ cases pin the text/token and
  bounded-tail fields.
- `mcp-request-cancellation` remains **partial**: the stdio cancellation notification reason does
  not match pi (`"cancelled"` vs the caller's reason or `"Request timed out"`). The HTTP timeout
  reason matches; the HTTP abort reason is a reviewed stop-token seam.

This ticket does not change capability membership and does not promote a row by itself. The
serial merger owns `audit_evidence.py register`, the consolidation `orphanScenarios` entries and
any `1058` delivery record build.

## Remaining boundary

The five captures are frozen and independently reproducible. The open work is the C++
`mcp-request-cancellation` stdio reason mismatch, plus the review decisions for the HTTP abort
reason and the two source-reviewed transport halves. `#1044` and `#1183` stay open. Magpie-only AI
access, XDG state, strict no-exception behavior, the ordinary application Session and the
no-historical-Pike-durable-migration boundary are unchanged.
