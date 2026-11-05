# #1181 MCP comparison reconciliation

## Scope

Reconcile the 18 supported `cch_mcp` capability rows with their current delivery evidence and
register complete whole-row comparison receipts where the full row is covered. Authority:
[#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044),
[#1183](https://github.com/lanshengzhi/cpp-coding-harness/issues/1183),
[ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md),
Named Baseline **pi-v1.3.0** at `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc.

Bounded verification scope: MCP protocol/client/transport, structured content and OAuth
cancellation/issuer/refresh/close semantics. Application management/binding belongs to the
application packet (#1177); the `mcp-management-*`, config, exposure and startup rows are not
`cch_mcp` rows.

This ticket implements no neighboring feature. Membership, implementation and verification stay
separate: a row advances only through a complete, pinned comparison receipt.

## Reconciliation method

1. Read every supported `cch_mcp` row in
   [1058-consolidated-capability-ledger.json](1058-consolidated-capability-ledger.json) and its
   recorded scenario, evidence route and remaining work. The source audit facts in
   [1052-mcp-audit-ledger.json](1052-mcp-audit-ledger.json) are preserved as history and were not
   treated as current code observations; several are now stale (see §Source-audit facts that are
   history).
2. For each row, find the delivered public C++ seam (`src/mcp`, `src/coding_agent/mcp`,
   `src/coding_agent/runtime/McpSessionManager`) and the frozen `audit-1052` scenario the audit
   registered. Reused the delivered #1065/#1066, #1101, #1102, #1103, #1104 and #1105 slices.
3. Compare the C++ case against the artifact's own observation fields through the highest
   appropriate Owner or application caller. Where a delivered case did not separate every recorded
   field, strengthen it in place instead of claiming a mapping it did not have.
4. A row is complete only when every recorded comparison scenario has a passing C++ case that
   observes the frozen behavior plus a passing discriminating case. Complete rows are registered in
   one pinned delivery record. Every other row keeps its recorded status and names a bounded
   follow-up.

## Delivered behavior

### Complete delivered rows (7 new)

One delivery record, `1181-mcp-comparisons`, in [1058-deliveries.json](1058-deliveries.json),
pins the production sources, comparison test sources, frozen artifacts, evidence tooling, the CTest
JUnit result and the resolved scenario routing. Each row's `remainingBoundary` is empty.

| Row | Frozen artifact | Scenarios compared | C++ case(s) |
| --- | --- | ---: | --- |
| `cch_mcp:mcp-http-transport` | `audit-1052/transports.json` | 5 | `an MCP HTTP URL must be http or https`<br>`streamable HTTP transport connects over plain http`<br>`connecting an MCP HTTP server lists its tools with the pi naming and schema`<br>`a same-origin redirect is followed and keeps the request auth header`<br>`cross-origin redirect is followed without Authorization and is rejected` (`tests/coding_agent/McpHttpSessionTest.cpp`) |
| `cch_mcp:mcp-oauth-issuer` | `audit-1052/oauth-lifecycle.json` | 1 | `an authorization code from another issuer never reaches the token endpoint` (`tests/coding_agent/McpOAuthFlowTest.cpp`) |
| `cch_mcp:mcp-request-auth` | `audit-1052/transports.json` | 3 | `streamable HTTP transport connects over plain http`<br>`a same-origin redirect is followed and keeps the request auth header`<br>`cross-origin redirect is followed without Authorization and is rejected` (`tests/coding_agent/McpHttpSessionTest.cpp`) |
| `cch_mcp:mcp-resources-list-read` | `audit-1052/application-initialization.json` | 3 | `agent session lists MCP resources only on demand and can call tools after a resource failure` (`tests/coding_agent/McpSessionIntegrationTest.cpp`) |
| `cch_mcp:mcp-oauth-discovery` | `audit-1052/oauth-lifecycle.json` | 1 | `a cancelled MCP OAuth discovery stops before any registration or token request` (`tests/coding_agent/McpOAuthSessionTest.cpp`) |
| `cch_mcp:mcp-oauth-flow-cancellation` | `audit-1052/oauth-lifecycle.json` | 3 | `a cancelled MCP OAuth discovery stops before any registration or token request`<br>`a cancelled MCP OAuth client registration stops before any token request` (`tests/coding_agent/McpOAuthSessionTest.cpp`)<br>`a browser callback wait cancelled by the sign-in stop token stops sign-in` (`tests/coding_agent/McpOAuthSignInTest.cpp`) |
| `cch_mcp:mcp-oauth-registration-signin` | `audit-1052/oauth-lifecycle.json` | 3 | `a cancelled MCP OAuth client registration stops before any token request` (`tests/coding_agent/McpOAuthSessionTest.cpp`)<br>`a browser callback wait cancelled by the sign-in stop token stops sign-in` (`tests/coding_agent/McpOAuthSignInTest.cpp`)<br>`an authorization code from another issuer never reaches the token endpoint` (`tests/coding_agent/McpOAuthFlowTest.cpp`) |

The existing `cch_mcp:mcp-tools-call-structured` receipt (`1101-structured-content`) remains valid
and is not claimed again.

### Strengthened cases

The delivered tests already cited the frozen scenarios, but three did not assert every recorded
field. This reconciliation strengthened them (comparison-only changes, no production behavior):

| Case | Change | Frozen fields now compared |
| --- | --- | --- |
| `an MCP HTTP URL must be http or https` | added both accepted config shapes | `http:` and `https:` are accepted at registration; `ftp:` is rejected with pi's exact message |
| `an authorization code from another issuer never reaches the token endpoint` | added expected/received issuer assertions | the error names both issuers; the POST log stays empty |
| `a browser callback wait cancelled by the sign-in stop token stops sign-in` | new case | authorization URL query keys, S256 challenge, state, loopback redirect and `resource`; registration ran; no token exchange |

The transports row's trusted-TLS half is observed by the fixture connections in
`connecting an MCP HTTP server lists its tools with the pi naming and schema` (the client trusts
only the committed test CA through `SSL_CERT_FILE`), and the untrusted half by the same fixture with
an empty CA store. The OAuth cancellation cases observe the flow's stop token at the scripted
`OAuthHttpClient` seam; the production `BoostBeastOAuthHttpClient` propagates that same token through
`CancellationSignalBridge` (source-reviewed). The report records that as a reviewed seam boundary,
not as a separate frozen socket observation; no claim is made that a cancelled socket has its own
registered C++ case.

### Revalidated existing receipt

The strengthened `tests/coding_agent/McpHttpSessionTest.cpp` is a pinned input of the
`1101-structured-content` record. Its 36-case selection was rerun at the test commit, the new JUnit
is [deliveries/1101-revalidation-1181.xml](deliveries/1101-revalidation-1181.xml), and the record
records a `revalidation1181` entry. The delivered rows, scenarios and routing digest are unchanged.

### Source-audit facts that are history

The 2026-10-19 audit predates the delivered slices. These recorded implementation statements were
not copied into the reconciliation:

- `mcp-http-transport` recorded a TLS-only gate and redirect refusal; #1103 delivered plain HTTP,
  same/cross-origin redirects and unchanged certificate validation.
- `mcp-oauth-flow-cancellation` recorded `missing`; #1102 delivered discovery, registration,
  callback and token cancellation.
- `mcp-resources-list-read` recorded a connect-time resource snapshot; #1105 removed it and lists on
  demand.
- `mcp-stdio-stderr-tail` recorded `missing`; #911 delivered the bounded 64 KiB ring and failure
  tail. The row stays unverified only because no frozen capture was registered.
- `mcp-client-lifecycle` recorded no background connect; #1104 delivered session creation that
  returns while a server is still connecting. The row is nevertheless routed to #1189 because the
  remaining close/refresh fields are not matched (below).

### Remaining obligations (10 rows)

Every remaining row names a bounded batch. No row is left to
[#1126](https://github.com/lanshengzhi/cpp-coding-harness/issues/1126), and no row moved to
Deferred.

**B1 — close and refresh lifecycle** ([#1189](https://github.com/lanshengzhi/cpp-coding-harness/issues/1189)).
Owner `cch_mcp`; public caller `Client`/`OAuthProvider`/`McpOAuthTokenResolver` through
`McpSessionManager`. Rows: `mcp-client-lifecycle`, `mcp-close-no-refresh`, `mcp-oauth-refresh`,
`mcp-refresh-cancellation`. Gaps: the pending-initialize close has no abort/settlement handle and no
observable closed error; C++ cancels an in-flight refresh instead of letting it complete and persist;
the per-server single-flight, 30-second skew and cross-process lock are absent; the
timeout-during-refresh comparison is missing. Scenarios are the four
`audit-1052/oauth-lifecycle.json` refresh/close scenarios and
`audit-1052/application-initialization.json` `closing while initialize is pending returns without
waiting`.

**B2 — roots/list and stdio server requests** ([#1190](https://github.com/lanshengzhi/cpp-coding-harness/issues/1190)).
Owner `cch_mcp`; public caller `Client` through the real stdio/HTTP fixtures. Row:
`mcp-roots-list`. Gaps: the HTTP root is derived from the process cwd instead of the connection,
stdio installs neither `ping` nor `roots/list`, and no frozen observation exists. The follow-up owns
the implementation, an independent frozen capture and the comparison.

**B3 — source-only protocol captures** ([#1191](https://github.com/lanshengzhi/cpp-coding-harness/issues/1191)).
Owner `cch_mcp`; public caller `Client` through the real stdio/HTTP fixtures. Rows:
`mcp-tools-list`, `mcp-progress`, `mcp-request-cancellation`, `mcp-http-session-stream`,
`mcp-stdio-stderr-tail`. All five implementations are delivered and have C++ regression cases, but
the audit registered no frozen scenario, so the rows cannot have a whole-row receipt. The follow-up
owns the independent captures and comparisons.

## Validation

Measured on this ticket's comparison commit `31fba00f6`:

| Command | Result |
| --- | --- |
| `cmake --build --preset vcpkg --target cch_tests_coding_agent` | built; build-phase Parity Architecture Gate PASS |
| `ctest --preset vcpkg -N -R '<16 exact MCP case names>'` | 16 selected |
| `ctest --preset vcpkg -R '<16 exact MCP case names>' --output-on-failure --output-junit docs/research/pi-v1.3.0/deliveries/1181-mcp.xml` | 16/16 passed, 0 failed, 0 skipped |
| `ctest --preset vcpkg -L '^(issue1090\|issue1101\|issue1104)$' --output-on-failure --output-junit docs/research/pi-v1.3.0/deliveries/1101-revalidation-1181.xml` | 36/36 passed, 0 failed, 0 skipped |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py build` | ledger rebuilt with 41 delivery-backed rows (7 from this record) |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check` | PASS — 1,464 rows; 568 scenarios, 362 named by a row, 56 artifact-covered, 150 explicit orphans, 0 uncovered |
| `python3 tests/evidence/ConsolidationAuditTest.py` | 42 tests, all passed |
| `python3 scripts/evidence/audit_evidence.py verify --baseline pi-v1.3.0` | 46 bundles, 102 artifacts and 565 scenarios verified at `71023aec…` |
| `cmake --build --preset vcpkg` then `ctest --preset vcpkg -j2` | 4,142 selected, 4,139 passed, 3 failed, 8 skipped; all 43 architecture cases passed |
| `scripts/format-check.sh 6764d6bcf` | clean |

The unfiltered full run's three failures are outside `cch_mcp` and were not caused by this
change:

- `bounded parallel cancellation leaves queued tool capabilities unstarted` (Agent tool executor)
  passed when rerun in isolation on the same tree.
- `Streaming assistant incremental block freeze maintains flat processing time across chunks`
  (the documented #632 quarantine benchmark) passed when rerun in isolation on the same tree; the
  unfiltered run was load-limited.
- `real PTY records Herdr markdown and OSC 52 terminal bytes` requires a Herdr-hosted PTY
  (`TERM_PROGRAM=herdr`); this host reports `ghostty`.

No assertion, timeout, label exclusion or normalization was changed to obtain that result, and the
eight skips supply no acceptance evidence.

The receipt pins the exact JUnit bytes, the selected count and the case names, so the scenario
mapping cannot drift silently. The selection command in the receipt records the exact set of case
names; the report lists every one in the complete-row table.

## Capability status

Exactly seven `cch_mcp` rows move from their recorded audit status to `implemented` / `verified`,
each through the validated delivery record: `mcp-http-transport`, `mcp-oauth-issuer`,
`mcp-request-auth`, `mcp-resources-list-read`, `mcp-oauth-discovery`,
`mcp-oauth-flow-cancellation` and `mcp-oauth-registration-signin`. With the existing
`mcp-tools-call-structured` receipt, `cch_mcp` has eight verified rows out of eighteen supported.
All other MCP rows keep their recorded implementation and verification status, their `auditStatus`
history and their named remaining work. Membership is unchanged: 18 supported and one excluded
`cch_mcp` row before and after; the excluded prompts/sampling surface keeps its boundary.

The whole ledger moves from 34 to 41 delivery-backed rows. This is a reconciliation count, not an
estimate of completion.

## Remaining boundary

Batches B1–B3 above are the complete remaining `cch_mcp` obligation in this packet. They must not
implement neighboring behavior, must capture any missing frozen observation instead of deriving it
from C++, and must not weaken a normalization. `#1044` and `#1126` stay open; closing this
verification packet accepts no adjacent row and no umbrella obligation.

Magpie-only AI access, XDG state roots, strict no-exception behavior, the ordinary application
Session, all explicit AI/platform exclusions and the no-historical-Pike-durable-migration boundary
remain unchanged. SIGKILL evidence does not establish power-loss or multi-process guarantees.

## Appendix — every supported `cch_mcp` row

Batches: B1 = #1189, B2 = #1190, B3 = #1191. The excluded prompts/sampling row keeps its existing
boundary and is not part of this reconciliation.

| Row | Capability | Disposition |
| --- | --- | --- |
| `cch_mcp:mcp-client-lifecycle` | Connect, initialize and close | B1 — pending-initialize close and refresh completion |
| `cch_mcp:mcp-close-no-refresh` | Close does not refresh and reuses the last token | B1 — refresh completion after close |
| `cch_mcp:mcp-http-session-stream` | Streamable HTTP session and server-to-client GET stream | B3 — capture + comparison |
| `cch_mcp:mcp-http-transport` | HTTP/HTTPS, redirects and TLS | **verified** — whole-row receipt |
| `cch_mcp:mcp-oauth-discovery` | Protected-resource and authorization-server discovery | **verified** — whole-row receipt (cancellation) |
| `cch_mcp:mcp-oauth-flow-cancellation` | Cancellation through discovery, registration and sign-in | **verified** — whole-row receipt |
| `cch_mcp:mcp-oauth-issuer` | RFC 9207 issuer verification | **verified** — whole-row receipt |
| `cch_mcp:mcp-oauth-refresh` | Refresh, rotation and cross-process locking | B1 — single-flight/skew/lock and completion |
| `cch_mcp:mcp-oauth-registration-signin` | Dynamic registration, PKCE and sign-in callback | **verified** — whole-row receipt |
| `cch_mcp:mcp-progress` | Progress notifications and deadline re-arming | B3 — capture + comparison |
| `cch_mcp:mcp-refresh-cancellation` | Cancellation during refresh never dispatches the request | B1 — refresh completion and timeout case |
| `cch_mcp:mcp-request-auth` | Request-time authentication and 401 handling | withdrawn by #1226 (sham discriminating case) — see `1202-withdrawn-1181-mcp-comparisons-claims.json`; the consolidated ledger reverts the row to source-audit status |
| `cch_mcp:mcp-request-cancellation` | Request cancellation and timeout | B3 — capture + comparison |
| `cch_mcp:mcp-resources-list-read` | Resource listing, templates and reads | **verified** — whole-row receipt |
| `cch_mcp:mcp-roots-list` | Server-to-client roots/list and ping | B2 — implementation + capture |
| `cch_mcp:mcp-stdio-stderr-tail` | Stdio failure diagnostics | B3 — capture + comparison |
| `cch_mcp:mcp-tools-call-structured` | Tools/call arbitrary structured content | **verified** — existing `1101-structured-content` receipt |
| `cch_mcp:mcp-tools-list` | Tools/list pagination and naming | B3 — capture + comparison |
| `cch_mcp:mcp-prompts-sampling` | Prompts, sampling and client-side request surfaces | excluded (unchanged) |
