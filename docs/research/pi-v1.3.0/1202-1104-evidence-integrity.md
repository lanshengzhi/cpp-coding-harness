# #1202 — independent check of the #1104 receipt

## Scope and disposition

At `acdafe873d2b125a0ce6e6a3773a07471b446b57`, inspect the two rows of receipt
`1104-management-during-connection`, their cited cases, and the actual frozen capture. This is a
bounded evidence assessment, not a change to capability membership, implementation status, the
receipt, or the ledger. Both rows currently claim `verified` with an empty `remainingBoundary`.
**Recommend that the serial merger downgrade their verification until the missing comparisons
below exist**; this is a recommendation, not a registered status change. Existing regression
assertions remain useful evidence of behavior, but do not establish the whole registered comparison.
Neither row is re-anchored in this slice: opening the file while retaining the current doubles would
leave material recorded observations untested.

## Provenance and data flow

The registry pins `audit-1104/manifest.json` to SHA-256 `a296b43eebec48c5c10512c4fa05e9c0e249bb3c5d74508bb5b671f7aa1d86d8`;
the manifest pins `management-during-connection.json` to
`af4058e65f92bb637e5a48e5af5ffac42baa2f7db1f49619613a0abeadf9fa7f` and pins the capture and
helper sources. The named baseline is pi-v1.3.0 at `71023aec1546d6d677a65669c5665c0927ce0dd0`.
`scenarios.mts` drives the frozen pi extension through a scripted extension host; the manager
scenario uses the real pi view on a headless terminal, an in-memory never-answering server and a
local OAuth HTTP fixture. The pickup scenario writes a credential store through a second store
instance and records server HTTP requests. It simulates an external login; it does not spawn another
process. `capture-1104.mts` writes those observations into the registered artifact.

The receipt cites one scenario per row. The cited C++ tests and their reachable local test doubles
use values constructed in C++; neither case loads the registered artifact or compares its
`expected` fields. This conclusion is based on tracing these concrete cases and their helpers,
not on a general claim that a source scan can resolve every indirect file read. A literal assertion
can still be a valid regression assertion; the evidence gap is the unmade comparison to the pinned
observation and the additional behavioral differences below.

## Row 95 — external sign-in pickup

The frozen scenario records `stateBeforeExternalSignIn`,
`stateAfterExternalSignInBeforeTurn`, `stateAfterTurnStart`, and `authorizedRequests`. The first
three are full status strings; the last is whether the OAuth fixture observed a successful HTTP
request. The cited `on_turn_start reconnects a server signed in through another process` case in
`tests/coding_agent/McpSessionManagerTest.cpp` starts a scripted connection in `NeedsAuth`, checks
one token snapshot and no reconnect after a pending-connect wait, mutates
`ScriptedAuth::stored_tokens_value` in memory, calls `on_turn_start`, and checks one
reconnect and `Connected`. The scripted reconnect sets the state itself; no credential file, HTTP
OAuth challenge, successful authorized request, full status text, or frozen expected field is
observed. In particular, the wait and zero-reconnect check occur **before** the simulated token
change, so they do not demonstrate that a status read after external sign-in leaves the state
unchanged. The companion unchanged-token test separates unconditional reconnect, but is also
scripted. The receipt discriminator's description of a store mutated from a second owner and tool
exposure is stronger than the cited pickup case's actual setup and assertions.

Follow-up: drive a real Session/manager and production credential store with a second store handle
and local OAuth HTTP fixture, record status after the write but before the turn, invoke a turn, and
compare the applicable status observations and authorized-request result to the named scenario's
frozen fields. Preserve the unchanged-token discriminator and confirm tool registration separately.
If Pike has no equivalent plain status surface, record the exact mapping or remaining boundary;
do not manufacture a literal status string as the observed result.

## Row 96 — management UI

The frozen scenario records `serversMenuWhileConnecting`, `authServerMenuItems`,
`authMenuStateAfterSignIn`, `signInMs`, `serversMenuAfterSignIn`, `slowServerMenuItems`, and
`slowMethods`. The cited `McpManagerFlowTest.cpp` case uses a gated `slow` and already connected
`fast`, checks screen substrings, opens the slow menu, and verifies a reconnect subcommand waits for
the gate. It does not select an auth server or sign in, compare any of those seven fields, observe
an initialize method, or measure the captured sign-in bound. The companion manager test holds
`slow` while a **scripted** sign-in for `auth` completes; it does not drive the panel, an OAuth HTTP
server, or compare the capture. These cases show useful liveness and ordering properties, not the
registered panel-and-sign-in observation.

Follow-up: drive the interactive management flow with a gated initialize and OAuth challenge, use
the frozen scenario's inputs and expected menu, state, method and bounded-time observations, and
compare actual output under explicit path/format normalization. Exercise the auth sign-in through
the panel while slow remains pending; reject a design that blocks the panel or waits for slow.
Time assertions must follow `CODING_STANDARDS.md` §11.9. If output presentation differs, record a
specific comparison boundary rather than normalize away the asserted behavior.

The separately captured plain `/mcp` status scenario is not listed in either row's
`comparisonScenarios`; #1104's delivery report says Pike print mode has no corresponding slash
command seam. It remains an explicit product/evidence boundary, not an additional scenario silently
assigned to these two receipt rows.

## Boundary check and other rows

A source-text search for an artifact path can catch a direct hard-coded read but cannot reliably
establish dynamic or indirect access, and reading a file alone does not prove its fields influenced
an assertion. It can also be fooled by an unused read. A feasible bounded check would run each
registered comparison in an isolated process against a controlled copy of its artifact, record
actual file access (including helper reads), and require the test to fail when a row-relevant frozen
expected value is mutated. Include controls for direct and indirect reads, no read, and an unused
read. Such a check requires a configurable fixture root, platform-specific access observation or
an explicit test-harness contract, and a row-specific meaningful mutation; shared scenario fields,
caches, alternate paths, or a test that fails for an unrelated reason require careful controls. It
would establish its declared cases, not prove all assertions are semantically correct. This slice
implements no general boundary test; #1202's boundary criterion remains open.

The remaining #1202 leads need separate bounded tracing before any status change:

- #1118 rows 63/68/69: trace the artifact-reading catalog comparisons and independently determine
  what the session discriminators establish; row 72 is listed as unaffected, but is not re-audited here.
- #1179: inspect all three timeout comparisons and the three worker cases against their actual
  receipt artifact and fields; #1202 names `audit-1056/worker-lifetime.json` while #1177 names
  `audit-1053/worker-lifetime.json`, so resolve from the receipt before choosing a path.
- #1181 and #1189: map each registered row to its precise OAuth or initialization scenario and
  trace the runtime fixtures and expected fields; do not assume a fixture read is a comparison.
- #1090: trace the image fixture helper and both receipt comparisons to the registered artifact;
  coordinate any shared receipt refresh with the serial merger. No #1158 Environment work is in scope.

No other row is declared accepted or downgraded by this report.

## Validation and handoff

- `python3 scripts/evidence/audit_evidence.py read --baseline pi-v1.3.0 --family audit-1104 --artifact management-during-connection.json` succeeded and emitted the registered bytes after the evidence-chain check.
- `sha256sum` of the manifest and artifact matched the pins above. `python3 scripts/evidence/audit_evidence.py verify --baseline pi-v1.3.0` passed: 57 bundles, 118 artifacts, 661 scenarios.
- This is a documentation-only assessment; no C++ test was changed or run, and no passing existing
  test is represented as a new artifact comparison. No full suite was run.
- PM owns any receipt/ledger downgrade, selection rerun, pinned evidence refresh and merge. #1202
  remains open; its acceptance criteria are not marked complete.
