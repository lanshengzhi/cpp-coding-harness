# #1189 MCP close and refresh lifecycle comparisons

## Scope

Deliver the remaining close and OAuth-refresh lifecycle comparisons for the `cch_mcp` owner
package, under [#1181](https://github.com/lanshengzhi/cpp-coding-harness/issues/1181)
(MCP comparison reconciliation), [#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044)
and [#1183](https://github.com/lanshengzhi/cpp-coding-harness/issues/1183). Authority:
[ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md),
Named Baseline **pi-v1.3.0** at `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with
glibc.

Four supported rows move through frozen `audit-1052` comparisons:

| Row | Frozen artifact | Scenarios |
| --- | --- | ---: |
| `cch_mcp:mcp-client-lifecycle` | `audit-1052/application-initialization.json`, `audit-1052/oauth-lifecycle.json` | 2 |
| `cch_mcp:mcp-close-no-refresh` | `audit-1052/oauth-lifecycle.json` | 1 |
| `cch_mcp:mcp-oauth-refresh` | `audit-1052/oauth-lifecycle.json` | 2 |
| `cch_mcp:mcp-refresh-cancellation` | `audit-1052/oauth-lifecycle.json` | 2 |

The public caller is `cch::mcp::Client` / `OAuthProvider` / `McpOAuthTokenResolver` through
`cch::coding_agent::runtime::McpSessionManager` and the application `McpServerConnection`. The
slice adds no second credential store and no new durable runtime; the stored state stays the
existing `mcp-auth.json` record.

## Delivered behavior

### One shared refresh per server and URL

`McpOAuthRefresh` is pi's `createMcpAuthProvider` `refreshing` promise: the first request starts
the one exchange for its server/URL, every later request joins it, and the operation itself owns
the store and provider rather than any caller.

- A request waiter observes the refresh only while its own stop token is live. Cancellation or the
  MCP request deadline settles the caller immediately with its `Cancelled`/`Timeout` error; the
  exchange keeps running and the request is never re-dispatched after the refresh completes.
- The refresh is never cancelled by a waiter. It re-reads the stored grant, refreshes with pi's
  request-time endpoint, and saves the rotated state before it answers, so a rotated refresh token
  cannot be lost when its original caller is gone.
- A failed refresh is reported to every waiter and is never retried inside the operation. An
  `invalid_grant` response remains the explicit re-login error and does not loop.
- `settled()` resolves once no refresh is in flight, the seam shutdown and the tests use to keep a
  rotated grant without making close wait.

`McpOAuthTokenResolver` uses the shared operation for both request-time resolution (inside pi's
30-second skew) and the `onUnauthorized` recovery after a 401. The five-minute Models margin is
replaced by pi's `REFRESH_SKEW_MS` (30 seconds) in the request resolver and in the sign-in flow
resolver alike.

### Cross-process refresh lock and rotated-grant reuse

`McpAuthStore::refresh_lock` takes an advisory `flock` on
`mcp-auth-refresh-<sha256(store key)[:16]>` next to `mcp-auth.json`, retrying for 25 seconds every
100 ms (pi `REFRESH_LOCK_WAIT_MS` / `REFRESH_LOCK_RETRY_MS`). The kernel releases the lock when its
holder exits, so pi's 20-second stale-lock takeover is unnecessary; the file is kept so waiters
never lock two inodes.

The lock is held from reading the tokens to saving the rotated ones. A process that acquires it
after another process refreshed re-reads the store and reuses the rotated access token when it
differs from the stale one, instead of refreshing again with a consumed refresh token. Concurrent
401s in one process share the same operation before the lock; concurrent processes serialize on
the lock and then reuse the winner's grant.

### Close reuses its last token and never refreshes

The HTTP transport's session `DELETE` asks its auth source with `RequestAuthPurpose::SessionClose`,
which returns the last access token the connection actually sent (pi `StreamableHttpTransport`
`lastToken`) and never starts a refresh. Closing therefore returns while a stalled refresh is
still answered, sends the pre-refresh token, and lets the in-flight refresh save the rotated grant
afterwards. The frozen close scenario's `closeMs`, `closeReturnedWhileRefreshStalled`,
`refreshRequests: 1`, `refreshCompleted: true` and `deleteToken` are all observed.

### Abortable initialize and pi's closed error

`Client` connect (`HttpClient::connect`, `StdioClient::connect`) and the manager's
`McpConnectionFactory::connect` seam take a `std::stop_token`. The production factory passes it
through to the transport handshake; the stdio handshake registers a stop callback that closes the
child, and both transports release a partly initialized client. When the stop token is requested,
the factory settles with pi's `MCP client is closed`.

`McpSessionManager::close()` requests that token for every server with a connect in flight. When
the aborted connect lands, `launch_start` records the wrapped pi message
`MCP server "<name>" failed to connect: MCP client is closed`; `servers()` publishes a `closed`
snapshot with no tools and that error. A factory that cannot be interrupted keeps the old
prompt-close-and-drop behavior and its healthy late connection is dropped without inventing a
closed error.

### Compared frozen fields and the C++ cases

| Row | Frozen scenario | Compared fields | C++ case |
| --- | --- | --- | --- |
| `cch_mcp:mcp-client-lifecycle` | `closing while initialize is pending returns without waiting` | `closeMs <= 250ms`, `state: closed`, `tools: []`, exact `pendingInitializeError` | `closing while a real initialize is pending returns with pi's closed error` (`tests/coding_agent/McpSessionManagerTest.cpp`) |
| `cch_mcp:mcp-client-lifecycle`, `cch_mcp:mcp-close-no-refresh` | `closing while a token refresh is in flight does not wait for the refresh` | `closeMs <= 250ms`, `closeReturnedWhileRefreshStalled`, `refreshRequests: 1`, `refreshCompleted: true`, pre-refresh `deleteToken` and the persisted rotated token | `closing an MCP HTTP client during a stalled OAuth refresh deletes the session with the stored token` (`tests/coding_agent/McpOAuthSessionTest.cpp`) |
| `cch_mcp:mcp-oauth-refresh`, `cch_mcp:mcp-refresh-cancellation` | `cancelling a tool call during OAuth refresh never sends it after the refresh completes` | one 401 `tools/call`, `refreshRequests: 1`, `refreshResponses: 1`, `toolCallsAfterRefresh: 0`, rotated `storedAccessTokenAfterRefresh` | `a cancelled or timed-out MCP OAuth tool call never retries after the shared refresh completes` (`tests/coding_agent/McpOAuthSessionTest.cpp`) |
| `cch_mcp:mcp-oauth-refresh`, `cch_mcp:mcp-refresh-cancellation` | `a tool call that times out during OAuth refresh never sends it after the refresh completes` | same fields with the request deadline's `Timeout` | same case (Catch2 `GENERATE`) |
| `cch_mcp:mcp-oauth-refresh` (capability) | recorded single-flight / 30-second skew / cross-process lock | one refresh for concurrent requests, no refresh outside the skew, lock wait and rotated-grant reuse | `concurrent MCP OAuth requests share one refresh`, `MCP OAuth refresh uses pi's 30-second skew`, `a token rotated by another process under the MCP OAuth refresh lock is reused without a second refresh` |

### Separating cases

- The pending initialize case drives a real never-answering `python3` child through
  `ProductionMcpConnectionFactory`: the child records its start, then reads stdin without answering
  `initialize`. Close returns before the child is aborted; the late settle publishes pi's exact
  closed error. The scripted `ConnectGate` case where the factory ignores the stop seam keeps the
  other half: prompt close, late connection dropped, no fabricated closed error.
- The close-during-refresh case observes the refresh still stalled at close
  (`post_responses == 0`), the pre-refresh `Bearer existing-access` DELETE, and only then releases
  the token endpoint and waits for the rotated grant. A per-request refresh or a close that waited
  for the exchange fails it.
- The shared-refresh case holds the token endpoint and cancels (or lets the 250 ms deadline fire):
  the caller settles with `Cancelled`/`Timeout` while the shared POST's stop token stays unset, the
  first `tools/call` is the only one (the 401 attempt), and the rotated token is stored. The old
  caller-aborts-the-refresh behavior fails this case.
- The cross-process case forks a real second process that holds the same lock file, then saves a
  rotated grant before releasing it. The waiting process uses the rotated token with zero refresh
  POSTs, separating the lock plus re-check from a per-request refresh that would send a second POST
  with the consumed refresh token.
- The skew case separates a request-time refresh (inside 30 seconds) from close's non-refresh: a
  two-minute token is sent untouched with no token request, while a twenty-second token refreshes.

## Validation

All commands ran in this worktree on Debug preset `vcpkg`, code revision
`6f0942cf7317a8cdab2db0fd3a5d15c6156f4028` (the delivery record pins the same revision and its
selected inputs).

| Command | Result |
| --- | --- |
| `cmake --build --preset vcpkg --target cch_tests_coding_agent` | built; build-phase Parity Architecture Gate PASS |
| `ctest --preset vcpkg -R '<9 exact case names>' --output-on-failure --output-junit docs/research/pi-v1.3.0/deliveries/1189-close-refresh.xml` | 9 selected, 9 passed, 0 failed, 0 skipped |
| `ctest --preset vcpkg -L '^(issue1090\|issue1101\|issue1104)$' ... 1189-revalidation-1090-1101-1104.xml` | 36/36 passed (shared #1101/#1104 receipt rerun) |
| `ctest --preset vcpkg -L issue1118 ... 1189-revalidation-1118.xml` | 19/19 passed |
| `ctest --preset vcpkg -R '<16 exact #1181 case names>' ... 1189-revalidation-1181.xml` | 16/16 passed |
| `cmake --build --preset vcpkg` then `ctest --preset vcpkg -j2` | 4,212 selected, 4,209 passed, 3 failed, 11 skipped; all 43 architecture cases passed |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py build` | rebuilt; 45 delivery-backed rows (4 from this record) |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check` | PASS — 1,464 rows; 614 scenarios, 362 named by a row, 56 artifact-covered, 196 explicit orphans, 0 uncovered |
| `python3 tests/evidence/ConsolidationAuditTest.py` | 42 tests, all passed |
| `python3 scripts/evidence/audit_evidence.py verify --baseline pi-v1.3.0` | 52 bundles, 108 artifacts and 617 scenarios verified at `71023aec…` |
| `scripts/format-check.sh origin/main` | clean |

The unfiltered run's three failures are outside the changed surface:

- `real PTY records Herdr markdown and OSC 52 terminal bytes` requires a Herdr-hosted PTY
  (`TERM_PROGRAM=herdr`); this host reports `ghostty`. It is the same environment limitation the
  [#1181](1181-mcp-reconciliation.md) receipt recorded.
- `cch_pi_v130_consolidation_check` and `cch_pi_v130_consolidation_boundaries` failed only because
  that run preceded the consolidation `build` of this record (the committed ledger still named the
  pre-#1189 inputs). Both were rerun after `build` and passed; `check` below is the final state.

No assertion, timeout, label exclusion or normalization was changed to obtain the result. The
eleven skips are the documented dual-runtime/PTY selections and supply no acceptance evidence.

## Capability status

The four `cch_mcp` rows move from `partial` / `upstream-captured` to `implemented` / `verified`
through the `1189-close-refresh` delivery record: `mcp-client-lifecycle`, `mcp-close-no-refresh`,
`mcp-oauth-refresh` and `mcp-refresh-cancellation`. With the eight rows #1181 and #1101 delivered,
`cch_mcp` has twelve verified rows out of eighteen supported. The whole ledger moves from 41 to 45
delivery-backed rows. Membership is unchanged: eighteen supported and one excluded `cch_mcp` row
before and after.

## Remaining boundary

The complete remaining `cch_mcp` obligation stays with the batches #1181 named: B2
([#1190](https://github.com/lanshengzhi/cpp-coding-harness/issues/1190), roots/list and stdio
server requests) and B3 ([#1191](https://github.com/lanshengzhi/cpp-coding-harness/issues/1191),
source-only protocol captures). This slice implements no adjacent behavior, captures no missing
frozen observation from C++, and leaves no row to
[#1126](https://github.com/lanshengzhi/cpp-coding-harness/issues/1126). `#1044` stays open.

The refresh lock is a same-host advisory `flock` on the XDG state directory; it replaces pi's
proper-lockfile stale takeover with the kernel's process-exit release and makes no network-file
system or multi-host claim. Magpie-only AI access, XDG state roots, strict no-exception behavior,
the ordinary application Session and the no-historical-Pike-durable-migration boundary remain
unchanged.
