# #1052 MCP protocol, OAuth and application integration audit

**Status:** audit complete. The frozen behavior is captured as `upstream-captured`; no row is
`verified`, because this ticket did not run a C++ parity suite and did not change production code.
This ticket did not perform the `cch_mcp` extraction or the OAuth/transport fixes.

- **Baseline:** `pi-v1.3.0` at `71023aec1546d6d677a65669c5665c0927ce0dd0`
- **Capture host:** Linux x86-64, glibc 2.44, Node v26.12.0, tsx 4.22.1
- **Private frozen checkout:** `/tmp/cch-1052-pi-frozen`; tracked tree clean and `HEAD` verified as
  the full baseline SHA before the capture wrote anything
- **Integration base:** `bf1b7c0d13f584c9ebae530ce7ab061f57e10c7f` (`integration/1044-pi-v1.3.0`,
  including the landed #1046/#1047/#1048/#1049 bundles)
- **Ticket branch:** `ticket/1052-audit`
- **Machine ledger:** [1052-mcp-audit-ledger.json](1052-mcp-audit-ledger.json) — 31 capability
  rows, one exclusion, the implementation/verification split, consumer chains and exact C++/upstream
  seams
- **Evidence family:** `fixtures/pi-v1.3.0/audit-1052/` — four artifacts, 25 scenarios
- **Capture source:** `fixtures/pi-v1.3.0/audit-1052/capture-1052.mts`,
  `scenarios.mts`, `fixtures.mts`, `pi.mts`

The report answers the A08 acceptance criteria. The JSON ledger is the authoritative inventory; the
report explains the observations, the boundaries and the narrow slices that follow.

## Evidence bundle

| Artifact | Scenarios | SHA-256 |
|---|---:|---|
| `structured-content.json` | 5 | `3061724d83492738e9f0d29fd089b5fc8d940be09901339a8183319637b204e5` |
| `oauth-lifecycle.json` | 7 | `21d20a43ddb027e1ce322d9632164776c7d6c8b5144ae8f01bee929a0eee2c47` |
| `transports.json` | 6 | `2edd0f5d56b27af96c06ccb6e295139ac58c898bc58c62500f69b3d461a66504` |
| `application-initialization.json` | 7 | `71d447812fc6fbf5c95ddc423552e8dd1fa1c879924886f6987d531876346cb8` |

`manifest.json` SHA-256: `15acbbfe81dbb9a226f6e5f963953b354b5823d892868a2d134fb5418c629eb9`.

The exact capture command (run from the repository root; the capture re-executes itself under the
frozen checkout's `tsx`):

```bash
NODE_OPTIONS=--conditions=source \
PI_CHECKOUT=/tmp/cch-1052-pi-frozen PI_BASELINE=pi-v1.3.0 \
  /tmp/cch-1052-pi-frozen/node_modules/.bin/tsx \
  fixtures/pi-v1.3.0/audit-1052/capture-1052.mts
```

`python3 scripts/evidence/audit_evidence.py check --manifest fixtures/pi-v1.3.0/audit-1052/manifest.json`
accepted the bundle and printed this registry record:

```json
{
  "audit-1052": {
    "manifest": "audit-1052/manifest.json",
    "manifestSha256": "15acbbfe81dbb9a226f6e5f963953b354b5823d892868a2d134fb5418c629eb9"
  }
}
```

Serial registration in `fixtures/pi-v1.3.0/baselines.json` is left to the parent/serial merger.
Until that registration lands, every row is `upstream-captured`, never `verified`.

### Frozen-checkout resolution topology

The private checkout is a copy at the exact revision, not the shared `../pi` working tree. Its
`node_modules` is a local directory of dependency symlinks into the shared install, with workspace
links pointing back into the **local** frozen `packages/*`. `@earendil-works/pi-mcp` and
`@earendil-works/pi-codemode` expose the frozen package.json `source` condition;
`NODE_OPTIONS=--conditions=source` selects their frozen TypeScript sources instead of any stale
copied `dist`. `TSX_TSCONFIG_PATH` points at the frozen root `tsconfig.json`, whose workspace paths
select local `packages/tui` sources. The only local builds are generated `dist` outputs for the
frozen MCP and TUI packages; they are gitignored and their inputs are the frozen sources. No shared
`node_modules`, shared package source or shared build output was edited.

The capture uses only local loopback fixtures, dummy `access-0`/`refresh-0` tokens and a temporary
self-signed loopback certificate. It does not read `~/.pi`, a provider catalog, a real token or a
network service.

## 1. Inventory and ownership boundary

The inventory keeps two layers distinct, as required for the later extraction:

| Layer | Frozen pi seam | Pike seam | Consumer / boundary |
|---|---|---|---|
| Generic MCP client | `packages/mcp/src/client.ts` `McpClient`; `protocol/{jsonrpc,types,content}.ts` | `McpServerConnection` interface, `McpStdioClient`, `McpHttpClient`, `McpProtocol.hpp` | Tool execution, codemode, resources, management; no application config or UI |
| Transports | `transports/stdio.ts`, `streamable-http.ts`, `in-memory.ts` | `McpStdioClient`, `McpHttpClient` over `StreamTransport`/Boost.Beast; in-memory only in tests | Generic client; callback and OAuth layers do not know the wire transport |
| Generic OAuth | `packages/mcp/src/oauth/{discovery,flow,provider,callback,types,errors}.ts` | `McpOAuthDiscovery`, `McpOAuthFlow`, `McpOAuthProvider`, `OAuthCallbackServer`, `McpOAuthSignIn` | Generic client request auth and application sign-in driver |
| Token persistence | Application `mcp-auth.json` store (`extensions/mcp/oauth.ts` `McpOAuthCredentialStore`) | `McpAuthStore`; `McpOAuthTokenResolver` | Application only; the generic client receives an auth provider |
| Application config | `extensions/mcp/config.ts`; `core/mcp-servers.ts` | `McpConfigFile`, `McpHttpServerConfig`, `McpExposure`, `McpConfigWrite` | `createMcpExtension`, `McpSessionManager`, CLI and panel |
| Connection lifecycle | `extensions/mcp/runtime.ts` `McpServerConnection` | `runtime/McpProductionAdapters.*` `ProductionMcpConnection`; `McpLiveConnection` | Application session bootstrap and `/mcp` |
| Exposure / discovery | `extensions/mcp/index.ts` `ensureDiscoveryActive`, `renderServersSection` | `McpSessionManager`, `McpServersSection`, `McpExposure` | Codemode, tool_search, prompt section |
| Management UI | `extensions/mcp/ui.ts` and the `/mcp` command in `index.ts` | `tui/McpManagerFlow`, `McpManagerPresenter`, `McpManagerView`; `McpSessionManager` actions | Interactive terminal and CLI |
| Session-tool conversion | `extensions/mcp/tools.ts` `convertMcpResult`, `createMcpToolName`, `createMcpResultSchema` | `McpExtensionToolSource`, `McpProductionAdapters::AgentMcpToolSurface`, `McpResourceTools` | Agent tool surface, codemode nested calls, renderers |

The proposed `cch_mcp` owner boundary therefore starts at `McpClient` + protocol + transports +
generic OAuth. `mcp.json`, `mcp-auth.json`, `/mcp`, exposure activation and session-tool binding are
application integrations, not part of the reusable client contract.

## 2. Structured results: arbitrary JSON is real

The frozen `McpClient` accepts `structuredContent` as **any** JSON value. The captured observations
are:

| Input | Frozen result |
|---|---|
| `content: []`, `structuredContent: [1, {"x": true}, null]` | array preserved; `content` remains `[]` |
| `content: []`, `structuredContent: 7` | primitive `7` preserved |
| `content: []`, `structuredContent: null` | `null` preserved |
| `content` absent, `structuredContent: {ok: true}` | missing `content` becomes `[]` |
| `content: [text]`, `isError: true`, `_meta` | error, content and metadata preserved |

`convertMcpResult` serializes the structured value into model text when there are no content blocks
(pretty JSON; `null` becomes `"null"`), keeps the whole `CallToolResult` in its script-facing
`structuredContent` channel, and keeps `isError` separate from protocol errors. The current Pike
converter instead rejects a missing/non-array `content` with
`sent a tools/call result without content` (`McpExtensionToolSource.cpp:73-75`), and its result
schema requires an object `content` array while leaving `structuredContent` untyped when no output
schema exists. An object-only fixture would not have exposed either difference; the captured
array/primitive/null/error cases do.

## 3. OAuth lifecycle, cancellation and close

The local OAuth fixture is both the MCP resource server and the authorization server. It can stall
one endpoint independently, issue rotating refresh tokens, and record every `/token`, `/register`,
`/authorize`, `tools/call` and DELETE request. The real frozen `McpClient`/`StreamableHttpTransport`
and the application `createMcpAuthProvider` were driven against it.

### Cancellation during refresh

Aborting a `tools/call` while `/token` was stalled produced:

```json
{
  "error": {"name": "AbortError", "message": "MCP request aborted"},
  "refreshRequests": 1,
  "refreshResponses": 1,
  "toolCalls": [{"method": "tools/call", "token": "access-0", "status": 401}],
  "toolCallsAfterRefresh": 0,
  "storedAccessTokenAfterRefresh": "access-1"
}
```

The timeout case (`timeoutMs: 250`) produced the same shape with `McpTimeoutError`. The only
`tools/call` that reached the server was the initial 401 attempt; after the refresh completed and
rotated the token, no second call was dispatched. The refresh response was still persisted
(`access-1`). This is the required distinction: cancellation stops the caller and dispatch, not the
already-issued refresh.

### Close without refresh

With the stored access token inside the refresh skew and `/token` stalled, `McpClient.close()`
returned within the measured `<=250ms` bound while the refresh was still blocked. The DELETE used
the **pre-refresh** token (`access-0`), and the refresh completed only after the test released it.
Close did not ask the auth provider for a new token and did not wait for the refresh.

### Issuer verification and discovery/registration/sign-in cancellation

| Scenario | Observation |
|---|---|
| Forged authorization response `iss` | `OAuthIssuerMismatchError` before token exchange; zero `/token` requests |
| Cancellation during protected-resource discovery | `AbortError: This operation was aborted`; stalled request socket destroyed; zero token/registration requests |
| Cancellation during dynamic client registration | `AbortError`; stalled `/register`; zero token requests |
| Cancellation while waiting for the browser callback | `McpSignInCancelledError: Sign-in cancelled`; one registration; zero token requests; PKCE S256 authorization URL shown |

The sign-in URL carries `response_type`, `client_id`, `code_challenge`, `code_challenge_method=S256`,
`redirect_uri`, `state` and `resource`. The capture records query keys and the callback URI without
recording the random verifier/state or the ephemeral port.

## 4. HTTP/HTTPS, redirects and request authentication

All measurements use real `StreamableHttpTransport` + `McpClient` over loopback fixtures:

| Scenario | Observation |
|---|---|
| Application config URL policy | `http://` and `https://` accepted; `ftp://` rejected with `url must be an http or https URL` |
| Plain HTTP | connects; initialize POST 200; `notifications/initialized` POST 202 |
| HTTPS with a trusted self-signed CA | connects; certificate SAN `IP:127.0.0.1` passed through an undici `Agent.connect.ca` |
| HTTPS with an untrusted certificate | connection fails; `TypeError: fetch failed`, cause `DEPTH_ZERO_SELF_SIGNED_CERT` |
| Same-origin 307 | followed; POST is re-sent to `/mcp`; `Authorization: Bearer static` preserved; connection succeeds |
| Cross-origin 307 | followed; the target receives **no** `Authorization`; target answers 401; client raises `McpAuthRequiredError` |

These are standard fetch redirect semantics, not an MCP-layer allowlist. The TLS observation is the
platform certificate-validation contract the parent requires to retain. The former Pike rules are
therefore the opposite of the frozen behavior:

- `McpHttpServerConfig.cpp:19` rejects every non-`https://` URL (“TLS-only”).
- `McpHttpClient.cpp:94-121` refuses every redirect (“following Location could downgrade”).
- `McpHttpSessionTest.cpp:396,445` asserts both restrictions.

## 5. Application initialization, resources and management

The application capture drives the public `createMcpExtension` factory with a minimal public
`ExtensionAPI`/`ExtensionContext` stub and the public `McpServerConnection` over a scripted
in-memory transport. It does not use a private scheduler or mutate a Session; the stub records the
same registration/notification surface a real Session provides. This was necessary because the
frozen `test/suite/harness.ts` path imports generated model-data JSON that is absent from the
private checkout. The client/transport/OAuth observations above are unaffected and use the real
frozen production classes.

| Scenario | Frozen observation |
|---|---|
| Connect with resources advertised | methods are `initialize`, `tools/list` only; `hasResources: true`; no `resources/list`; tools register |
| Resources on demand | `resources/list` appears only after `allResources()`; tools remain available |
| `resources/list` failure | connection stays `connected`, tools remain; the resource call rejects with `McpError: resource boom` |
| Close while initialize is pending | `closeMs <=250ms`; state `closed`; pending initialize rejects with `MCP client is closed` |
| First prompt, codemode server still initializing | prompt returns within `<=250ms` (the 10 s default is not spent); `mcp_servers` section names `mcp__slow`; codemode active |
| First prompt, direct server still initializing | waits the requested 40 ms startup bound; emits “MCP servers are still connecting; their tools become available once connected.”; no tools declared |
| Direct server connects within the bound | prompt returns within `<=250ms`; `mcp__slow__echo` is registered |

The required application distinctions are therefore:

- connecting lists **tools only**; resources are lazy;
- a resource-list failure is local to the resource operation;
- close does not wait for a pending initialize;
- a codemode/deferred server never holds the first prompt;
- only direct servers participate in the bounded first-prompt wait;
- a server that connects inside the wait contributes its tools to that prompt.

Pike currently violates the first, second and startup distinctions: `ProductionMcpConnection::refresh`
calls `fetch_mcp_resource_snapshot` during connect and fails the connection if the snapshot fails,
and `McpSessionManager::start` awaits every enabled server serially before session creation. The
existing `McpSessionIntegrationTest.cpp:237` even asserts that the connect phase resolved before
creation, so a future test must change this expectation, not merely add a new assertion.

## 6. Capability ledger summary

The machine ledger has 31 rows. Counts by status:

| Status | Count | Rows |
|---|---:|---|
| implemented | 11 | tools listing, resource tools, progress, request cancellation, issuer, persistence, HTTP session/GET, management, session-tool binding, reconnect, deferred tool_search (some are implementation-complete but unverified) |
| partial | 15 | client lifecycle, structured results, conversion, resources, roots, refresh cancellation, close, discovery, sign-in, refresh, config, HTTP transport, request auth, exposure, resource init |
| missing | 4 | application startup wait/background connect, OAuth flow cancellation, external sign-in pickup, stdio stderr tail |
| excluded | 1 | prompts/sampling/client-side request surfaces (not exposed by v1.3.0 `McpClient`) |

No row is `verified`. “Implemented” here means the C++ seam exists and has tests, not that the
v1.3.0 differential has run. The important gaps are:

1. **Application startup is synchronous and unbounded.** `McpSessionManager::start` awaits every
   enabled server; there is no background connect, no `startupWaitMs`, and no still-connecting
   notification. This is the largest application behavior gap.
2. **Resources are eager and fatal at connect.** Production `refresh` snapshots resources before
   reporting connected and fails the connection on a list error; v1.3.0 sets `hasResources` from
   capabilities and lists on demand.
3. **HTTP is TLS-only and redirect-free.** Both restrictions are asserted by current tests and
   must be removed while retaining platform TLS validation.
4. **OAuth refresh is per-request, not single-flight.** Pike uses a five-minute refresh margin, no
   coalescing, no cross-process lock and no settled contract; cancellation waits for the refresh
   before suppressing dispatch instead of settling immediately. Token rotation persistence exists.
5. **OAuth discovery/registration/sign-in has no caller cancellation.** All flow HTTP calls use an
   empty `std::stop_token`.
6. **Structured results require `content` and lose the separate script/renderer channels.** The
   converter rejects missing content and puts the whole CallToolResult into one `details` value.
7. **Roots/ping and stdio diagnostics are incomplete.** HTTP hardcodes the cwd root; stdio has no
   `roots/list`/`ping` handler and drains stderr to `/dev/null`.
8. **External sign-in pickup is absent.** Only Pike's own `/mcp` sign-in triggers reconnect.

## 7. Former Pike restrictions to remove, and what to keep

`docs/adr/0054-make-client-transports-tls-only-with-concrete-executor-beast-streams.md` records the
old TLS-only client-transport decision. For MCP its URL/redirect clauses are superseded by v1.3.0
(ADR 0075 / this audit), while its certificate-validation and single-transport-stack concerns remain
valid:

- Remove the `https://`-only registration gate in `McpHttpServerConfig.cpp:19`; accept `http` and
  `https` and reject other schemes with the captured text.
- Remove the redirect refusal in `McpHttpClient.cpp:94-121`; follow standard fetch redirects,
  preserving same-origin Authorization and allowing the platform to strip it cross-origin. Do not
  add an MCP-specific redirect allowlist that would diverge from the captured behavior.
- Keep platform certificate validation. The untrusted-certificate case must still fail with the
  underlying TLS error; do not disable verification to make a self-signed fixture pass.
- Do not change the Magpie/provider TLS or `cch_ai` transport contract. This is an MCP transport
  change only.

The same sweep should replace the stale v1.0.4 assumptions in the MCP audit comments and tests:
`McpSessionManager`'s serial connect, `ProductionMcpConnection`'s eager resource snapshot, the
five-minute refresh margin, and the `McpHttpSessionTest` redirect/TLS-only assertions.

## 8. Recommended narrow slices for `cch_mcp` extraction

### Slice A — generic MCP client, protocol and transports

Own `McpClient`, JSON-RPC validation, `Tool`/`Resource`/callback types and the stdio/HTTP
transports. Dependencies: `support` async/JSON/stream and the platform TLS stack; **no**
coding-agent config, UI, `mcp-auth.json` or session types. Distinguishing cases before extraction:
the captured structured array/primitive/null/error results; plain HTTP; same- and cross-origin
redirects; untrusted TLS; cancellation/timeout; and close during a pending initialize. The current
C++ seams are already close to this boundary, but `McpHttpClient` is coupled to the application
`McpRequestAuthSource` and `McpHttpServerConfig`; split the auth-provider interface into the generic
owner and leave `mcp-auth.json` in coding-agent.

### Slice B — generic OAuth (discovery, registration, PKCE, issuer, refresh)

Own discovery, flow, provider, callback and errors. Dependencies: Slice A + a generic HTTP fetcher.
Distinguishing cases: forged `iss` rejected before token exchange; cancellation during discovery,
registration and browser/paste wait; refresh single-flight; cancelled/timeout caller never dispatches
the tool call after refresh; close never starts or waits for refresh; rotating refresh token
persisted. Keep `McpOAuthCredentialStore`/`McpAuthStore` and `mcp-auth.json` in coding-agent; expose
a generic state-store and lock interface from `cch_mcp`.

### Slice C — application connection/startup/resources

Own `McpServerConnection` state, background connect, retry/backoff, `startupWaitMs`, lazy resources,
reconnect, external-sign-in pickup and stderr tail. Dependencies: Slice A + application
config/store. Distinguishing cases: never-answering direct server (bounded prompt, notification),
never-answering codemode server (prompt does not wait), delayed direct server (tool appears),
resource-list failure (connection stays connected), close while initialize pending, and an external
token change between turns. This slice must change `McpSessionManager::start` and
`ProductionMcpConnection::refresh`; it cannot be completed by adding tests around the current
synchronous model.

### Slice D — result conversion and session-tool binding

Own `convertMcpResult`, `createMcpResultSchema`, the separate `details`/`structuredContent` channels,
resource-link/binary rendering, 20 KiB spill and `AgentMcpToolSurface`. Dependencies: Slice A result
types + coding-agent tool/Agent surface. Distinguishing cases: array/primitive/null structured
results, missing `content`, error results, resource-link and binary resources, and codemode receiving
the full CallToolResult minus `_meta`. Keep naming/sanitization with the session-tool adapter, not
the generic client.

### Slice E — application config, exposure and management

Own `mcp.json` loading/writing, URL validation, exposure activation, `mcp_servers` section and the
`/mcp` panel/CLI. Dependencies: Slice C + Slice D + codemode/tool_search. Distinguishing cases:
`http`/`https`/`ftp` URL policy; codemode vs direct vs deferred activation; status while connecting;
resource-tool exposure; and `/mcp` actions while a connection is still pending. Deferred exposure
and `tool_search` landed with #1095: activation, repeated searches in one run, the persisted loadout
and fork/reopen replay.

## 9. Validation and handoff

- Evidence: `python3 scripts/evidence/audit_evidence.py check --manifest fixtures/pi-v1.3.0/audit-1052/manifest.json`
  accepted the unregistered bundle and printed the registry record above.
- No shared registry, verifier, README, `tests/evidence/` or other audit directory was edited.
- No production C++/OAuth/transport changes were made.
- The complete handoff, exact commands, environment and registry patch are in
  `/tmp/cch-1044-context/1052-handoff.json` and the authoritative output handoff.
- Open limitation: the application scenarios use the public `createMcpExtension` boundary with a
  minimal `ExtensionAPI` stub, not a full `AgentSession` harness. A later test slice should re-run
  the three startup/resource cases through a real application Session once the frozen model-data
  harness is runnable; the public extension is the same boundary the real session calls, but the
  session-level wiring remains uncaptured.
