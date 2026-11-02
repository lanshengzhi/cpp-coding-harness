# #1179 codemode comparison reconciliation

## Scope

Reconcile all 49 supported `cch_codemode` capability rows with current delivery evidence and
register complete whole-row comparison receipts where the full row is covered. Authority:
[#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044),
[#1183](https://github.com/lanshengzhi/cpp-coding-harness/issues/1183),
[ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md),
Named Baseline **pi-v1.3.0** at `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc.

Bounded verification scope: reusable declaration/parser/globals/output, worker/Inline lifecycle,
deterministic outcome/replay and generic Remote behavior. The application-owned codemode catalog,
loadout, session store and result presentation rows belong to the application packet (#1177); the
two excluded rows (`models.*` provider execution and the Cloudflare Durable Object wrapper) keep
their existing boundaries.

This ticket implements no neighboring feature. Membership, implementation and verification stay
separate: a row advances only through a complete, pinned comparison receipt. The 49-row count is a
reconciliation inventory, not a completion percentage.

## Reconciliation method

1. Read every supported `cch_codemode` row in
   [1058-consolidated-capability-ledger.json](1058-consolidated-capability-ledger.json) and its
   recorded scenario, evidence route and remaining work. The source audit facts in
   [1053-codemode-audit-ledger.json](1053-codemode-audit-ledger.json) and the 1056 planning row are
   preserved as history and were not treated as current code observations.
2. Locate the delivered public C++ seam for each row (`src/codemode`, `src/coding_agent/extensions/codemode`)
   and the frozen `audit-1053` scenario the audit registered. Reused the delivered #1064, #1091,
   #1092, #1093, #1096, #1097, #1098, #1099 and #1100 slices.
3. Compare the C++ case against the artifact's own observation fields through the highest
   appropriate public Owner caller. Where a delivered case did not separate every recorded field,
   strengthen it in place instead of claiming a mapping it did not have.
4. A row is complete only when every recorded comparison scenario has a passing C++ case that
   observes the frozen behavior plus a passing discriminating case. Complete rows are registered in
   one pinned delivery record. Every other row keeps its recorded status and names a bounded
   follow-up.

## Delivered behavior

### Complete delivered rows (39 new)

One delivery record, `1179-codemode-comparisons`, in [1058-deliveries.json](1058-deliveries.json),
pins the production sources, comparison test sources, frozen artifacts, the CTest JUnit result and
the resolved scenario routing. Each row's `remainingBoundary` is empty.

The selected CTest set is exactly 52 cases and all 52 passed. The record's JUnit is
[deliveries/1179-codemode.xml](deliveries/1179-codemode.xml).

#### Source and options

| Row | Frozen artifact | Scenarios | C++ case(s) |
| --- | --- | ---: | --- |
| `cch_codemode:codemode-source-constants` | `audit-1053/source-options.json` | 1 | `the public codemode source parser keeps only a first-line options command`<br>`the public codemode source parser rejects a misplaced options line with its line number` |
| `cch_codemode:codemode-source-parse` | `audit-1053/source-options.json` | 4 | `the public codemode source parser keeps only a first-line options command`<br>`the public codemode source parser rejects a misplaced options line with its line number` |
| `cch_codemode:codemode-source-placement-errors` | `audit-1053/source-options.json` | 2 | `the public codemode source parser rejects a misplaced options line with its line number`<br>`the public codemode source parser rejects invalid options and empty input with typed errors`<br>`the public codemode source parser keeps only a first-line options command` |
| `cch_codemode:codemode-source-timeout-option` | `audit-1053/script-execution.json` | 2 | `an over-limit script is interrupted with a timeout error`<br>`a public codemode caller reports a script error with its name message stack and prior output`<br>`a public codemode caller reports the deadline when a tool waits past it and keeps partial output` |

#### Declarations

| Row | Frozen artifact | Scenarios | C++ case(s) |
| --- | --- | ---: | --- |
| `cch_codemode:codemode-declaration-identifier` | `audit-1053/declarations.json` | 1 | `the public codemode declaration preserves the relocated tool identifiers` |
| `cch_codemode:codemode-declaration-input-budget` | `audit-1053/declarations.json` | 1 | `the public codemode declaration renders frozen tool signatures samples and blocks`<br>`the public codemode declaration covers the frozen schema renderings` |
| `cch_codemode:codemode-schema-to-type` | `audit-1053/declarations.json` | 9 | `the public codemode declaration renders frozen schema types`<br>`the public codemode declaration covers the frozen schema renderings`<br>`the public codemode declaration renders frozen tool signatures samples and blocks` |
| `cch_codemode:codemode-render-tool-signature` | `audit-1053/declarations.json` | 2 | `the public codemode declaration renders frozen tool signatures samples and blocks`<br>`the public codemode declaration groups tools globals and namespaces with doc comments`<br>`the public codemode declaration renders frozen schema types` |

#### Script execution

| Row | Frozen artifact | Scenarios | C++ case(s) |
| --- | --- | ---: | --- |
| `cch_codemode:codemode-script-globals` | `audit-1053/deterministic-replay.json`<br>`audit-1053/script-execution.json` | 3 | `a public codemode caller binds a dotted global into its frozen namespace and spreads arguments`<br>`a public codemode caller rejects invalid globals at construction with the frozen errors`<br>`a public codemode caller numbers globals with tools and gets rejection properties on the script Error` |
| `cch_codemode:codemode-script-store-load` | `audit-1053/script-execution.json` | 2 | `a public codemode caller reads a store snapshot across calls and drops failed writes`<br>`a public codemode caller treats exit() as success with prior output and the write set` |
| `cch_codemode:codemode-script-output-text-image-console` | `audit-1053/script-execution.json` | 3 | `a public codemode caller collects mixed output in order with the console marker and image MIME`<br>`a public codemode caller stringifies text and accepts image blocks with the frozen type errors` |
| `cch_codemode:codemode-script-image-format` | `audit-1053/script-execution.json` | 2 | `a public codemode caller stringifies text and accepts image blocks with the frozen type errors`<br>`a public codemode caller collects mixed output in order with the console marker and image MIME` |
| `cch_codemode:codemode-script-exit` | `audit-1053/script-execution.json` | 1 | `a public codemode caller treats exit() as success with prior output and the write set`<br>`a public codemode caller reads a store snapshot across calls and drops failed writes` |
| `cch_codemode:codemode-script-error-taxonomy` | `audit-1053/script-execution.json` | 4 | `a public codemode caller reports a script error with its name message stack and prior output`<br>`a public codemode caller reports non-Error throws BigInt returns and syntax errors as script errors`<br>`a public codemode caller reports guest memory exhaustion as an out-of-memory script error`<br>`a public codemode caller catches unknown tools and thrown tool errors with error call status` |
| `cch_codemode:codemode-script-output-limits` | `audit-1053/script-execution.json` | 1 | `the public codemode prelude is the frozen sandbox prelude with output and store limits`<br>`a public codemode caller fails the run when the guest hits its output item limit` |
| `cch_codemode:codemode-script-unawaited-calls` | `audit-1053/script-execution.json` | 1 | `a public codemode caller records an unawaited call as cancelled and aborts it`<br>`a public codemode caller records settled calls with ok and error statuses` |
| `cch_codemode:codemode-script-memory-limit` | `audit-1053/script-execution.json` | 1 | `a public codemode caller reports guest memory exhaustion as an out-of-memory script error`<br>`a public codemode caller reports the deadline when a tool waits past it and keeps partial output` |
| `cch_codemode:codemode-script-abort-reason` | `audit-1053/deterministic-replay.json`<br>`audit-1053/script-execution.json` | 3 | `a public codemode caller aborting while a tool waits keeps partial output and cancels the call`<br>`a public codemode caller reports a pre-aborted run with the abort reason and never starts it`<br>`a public codemode caller gets an uncatchable abort and a cancelled record in a deterministic run` |

#### Deterministic replay

| Row | Frozen artifact | Scenarios | C++ case(s) |
| --- | --- | ---: | --- |
| `cch_codemode:codemode-deterministic-start` | `audit-1053/deterministic-replay.json` | 3 | `a public codemode caller replays a deterministic run from the same start seed and answers`<br>`a public codemode caller applies reversed answers by result number not arrival order`<br>`a run-supplied jsName overrides the derived script name` |
| `cch_codemode:codemode-deterministic-clock-seed` | `audit-1053/deterministic-replay.json` | 2 | `a public codemode caller gets UTC and no WeakRef in a deterministic run`<br>`a public codemode caller sees a seed mismatch change randomness without changing the start time` |
| `cch_codemode:codemode-deterministic-seq` | `audit-1053/deterministic-replay.json` | 3 | `a public codemode caller applies reversed answers by result number not arrival order`<br>`a public codemode caller rejects a reused result number a non-finite clock and an unknown call deterministically`<br>`a public codemode caller keeps a call waiting when its answer cannot be serialized` |
| `cch_codemode:codemode-deterministic-during` | `audit-1053/deterministic-replay.json` | 2 | `a public codemode caller gets during for the result the VM was processing when the script ended`<br>`a normal codemode run is not deterministic while start is` |
| `cch_codemode:codemode-deterministic-fingerprint` | `audit-1053/deterministic-replay.json` | 1 | `a public codemode caller gets the fingerprint before the first call and a reused result number is refused`<br>`a public codemode caller sees a seed mismatch change randomness without changing the start time` |
| `cch_codemode:codemode-deterministic-validation` | `audit-1053/deterministic-replay.json` | 3 | `a public codemode caller gets illegal start options refused before any VM exists`<br>`a public codemode caller sees a seed mismatch change randomness without changing the start time`<br>`a public codemode caller gets an uncatchable abort and a cancelled record in a deterministic run` |
| `cch_codemode:codemode-deterministic-properties` | `audit-1053/deterministic-replay.json` | 1 | `a public codemode caller numbers globals with tools and gets rejection properties on the script Error`<br>`a public codemode caller binds a dotted global into its frozen namespace and spreads arguments` |
| `cch_codemode:codemode-deterministic-vs-execute` | `audit-1053/deterministic-replay.json` | 1 | `a normal codemode run is not deterministic while start is`<br>`a public codemode caller replays a deterministic run from the same start seed and answers` |

#### Inline execution

| Row | Frozen artifact | Scenarios | C++ case(s) |
| --- | --- | ---: | --- |
| `cch_codemode:codemode-inline-success` | `audit-1053/inline-interrupt.json` | 1 | `a public codemode caller returns its value and text output from a successful budgeted run` |
| `cch_codemode:codemode-inline-interrupt-budget` | `audit-1053/inline-interrupt.json` | 2 | `a public codemode caller stops pure computation with the interrupt budget, not the deadline`<br>`a public codemode caller counts the interrupt budget across tool waits`<br>`a public codemode caller reports the deadline when a tool waits past it and keeps partial output` |
| `cch_codemode:codemode-inline-deadline-abort` | `audit-1053/inline-interrupt.json` | 3 | `a public codemode caller aborting while a tool waits keeps partial output and cancels the call`<br>`a public codemode caller reports the deadline when a tool waits past it and keeps partial output`<br>`a public codemode caller reports a pre-aborted run with the abort reason and never starts it`<br>`a public codemode caller stops pure computation with the interrupt budget, not the deadline` |

#### Remote

| Row | Frozen artifact | Scenarios | C++ case(s) |
| --- | --- | ---: | --- |
| `cch_codemode:codemode-remote-exchange` | `audit-1053/remote-exchange.json` | 2 | `RemoteCodemodeSandbox runs tools in the caller and the script on the remote`<br>`RemoteCodemodeSandbox batches output instead of making one exchange per line` |
| `cch_codemode:codemode-remote-output-batching` | `audit-1053/remote-exchange.json` | 1 | `RemoteCodemodeSandbox batches output instead of making one exchange per line`<br>`RemoteCodemodeSandbox runs tools in the caller and the script on the remote` |
| `cch_codemode:codemode-remote-caps` | `audit-1053/remote-exchange.json` | 1 | `RemoteCodemodeSandbox caps the interrupt budget and memory at the remote's limits` |
| `cch_codemode:codemode-remote-abort-stop` | `audit-1053/remote-exchange.json` | 1 | `RemoteCodemodeSandbox tells the remote to stop when caller aborts and settles immediately`<br>`RemoteCodemodeSandbox discriminating abort check: does not block on slow remote exit` |
| `cch_codemode:codemode-remote-failures` | `audit-1053/remote-exchange.json` | 1 | `RemoteCodemodeSandbox reports failing, silent or unreachable remote as sandbox error with transport reason` |
| `cch_codemode:codemode-remote-protocol` | `audit-1053/remote-exchange.json` | 1 | `RemoteCodemodeSandbox runs tools in the caller and the script on the remote`<br>`Remote deterministic execution runs the script remotely and answers through deliver` |
| `cch_codemode:codemode-remote-deterministic` | `audit-1053/remote-exchange.json` | 1 | `Remote deterministic execution applies reversed answers by result number`<br>`Remote deterministic execution runs the script remotely and answers through deliver` |

#### Worker lifecycle

| Row | Frozen artifact | Scenarios | C++ case(s) |
| --- | --- | ---: | --- |
| `cch_codemode:codemode-worker-admission` | `audit-1053/worker-lifetime.json` | 3 | `codemode worker results settle before exit while admission stays bounded`<br>`codemode worker admission follows wasm load and releases a failed start once` |
| `cch_codemode:codemode-worker-repeated-cancellation` | `audit-1053/worker-lifetime.json` | 1 | `codemode worker results settle before exit while admission stays bounded`<br>`codemode worker admission follows wasm load and releases a failed start once` |
| `cch_codemode:codemode-worker-construction-failure` | `audit-1053/worker-lifetime.json` | 2 | `codemode worker admission follows wasm load and releases a failed start once`<br>`codemode worker results settle before exit while admission stays bounded` |

### Strengthened and new cases

The delivered tests cited the frozen scenarios, but seven observations were not represented by a
case. This reconciliation changed comparison-only tests; no production behavior changed.

| Case | Change | Frozen fields now compared |
| --- | --- | --- |
| `an over-limit script is interrupted with a timeout error` | exact message instead of a substring | `Execution timed out after 150 ms` for a computing script |
| `the public codemode prelude is the frozen sandbox prelude with output and store limits` | new case | byte equality with `script-execution.json`'s `PRELUDE_SOURCE`, including the output/store limits |
| `a public codemode caller stringifies text and accepts image blocks with the frozen type errors` | new case | non-string `text()` stringification, the object image-block form, and the catchable `TypeError` |
| `a public codemode caller catches unknown tools and thrown tool errors with error call status` | new case | unknown-tool `TypeError`, thrown tool `Error`, one `error` call record with a duration |
| `a run-supplied jsName overrides the derived script name` | new case | `typeof tools.renamed`, `Object.keys(tools)` order and `ALL_TOOLS` names |
| `a normal codemode run is not deterministic while start is` | new case | real wall clock, changed randomness per run and no `during` on `run` |
| `RemoteCodemodeSandbox runs tools in the caller and the script on the remote` | added the terminal write set | empty `storeWrites` on the successful generic exchange |

### Source-audit facts that are history

The 2026-10-19 audit predates the delivered slices. These recorded implementation statements were
not copied into the reconciliation:

- The nine deterministic rows recorded `missing`; #1093 delivered `start`/`deliver`, the virtual
  UTC clock, seeded randomness, fingerprint, `during`, ordered delivery and rejection properties.
  #1099 added the deterministic Remote transport path.
- `codemode-remote` recorded `missing` in the 1056 planning lane; #1098 delivered
  `CodemodeRemote`, `RemoteCodemodeSandbox` and `serve_codemode_remote` with a Linux host.
- `codemode-source-output-token-option` recorded `missing`; #1119 delivered the output formatter
  with the budget, head/tail truncation, spill file and `fullOutputPath`.
- The Inline rows recorded `missing`; #1097 delivered the interrupt budget, shared deadline,
  caller abort reason and pre-abort no-VM guarantee on the public sandbox. The Inline destination
  itself is still a bounded gap below.
- The script globals/store/output/worker rows recorded several missing semantics; #1091, #1092,
  #1096 and #1097 delivered them and the tests above compare them.

These are current-code observations, not rewrites of the registered capture payloads. The
`auditStatus` retained by a delivered row preserves the prior consolidated status.

### Remaining obligations (10 rows)

Every remaining row names a bounded batch. No row is left to
[#1126](https://github.com/lanshengzhi/cpp-coding-harness/issues/1126), and no row moved to
Deferred.

**B1 — public tool table and close surface** ([#1192](https://github.com/lanshengzhi/cpp-coding-harness/issues/1192)).
Owner `cch_codemode`; public caller `CodemodeSandbox`/`RemoteCodemodeSandbox`. Rows:
`codemode-execute-outcome`, `codemode-inline-close`, `codemode-worker-close`. The shared
`script-execution.json` scenario `the public tool table can be registered, listed and unregistered;
close rejects later runs` is not represented: C++ has no register/list/unregister API and the
later-run error is `Sandbox closed` rather than `Sandbox is closed`. Normal case: duplicate
constructor tools, duplicate registration message, list, remove existing/missing, list after
removal, closed error. Discriminating case: register/list/unregister plus a later run, so a
close-only implementation fails.

**B2 — source-only captures** ([#1193](https://github.com/lanshengzhi/cpp-coding-harness/issues/1193)).
Owner `cch_codemode`; public caller `RemoteCodemodeSandbox`, `CodemodeSandbox::create` and the
application `executeCodemode` wrapper. Rows: `codemode-remote` (no scenario), 
`codemode-sandbox-create-errors` (no scenario) and `codemode-source-output-token-option` (no
scenario). The implementations are delivered, but a whole-row receipt needs an independent frozen
capture. Normal/discriminating cases are named in the issue: the generic Remote exchange and its
terminal write set; eager create failure versus lazy source-holder reuse; the output budget,
zero-budget path and spilled full text.

**B3 — portable no-Node entry** ([#1194](https://github.com/lanshengzhi/cpp-coding-harness/issues/1194)).
Owner `cch_codemode`; public caller the portable target. Row: `codemode-portable-entry`. The
extracted owner still links WasmEdge/Asio/threads and has no portable target; the row's frozen
scenario is the public options/grammar constants. Discriminating case: a build-level no-Node /
no-native check, not a header-existence assertion.

**B4 — discovery absence and host timezone** ([#1195](https://github.com/lanshengzhi/cpp-coding-harness/issues/1195)).
Owner `cch_codemode`; public caller scripts through the discovery globals and a normal `run`.
Rows: `codemode-script-discovery` (absent tool/namespace is JSON `null`, frozen pi is `undefined`)
and `codemode-script-clock-timezone` (C++ always reports UTC; frozen normal execution reads the
host timezone). The issue requires either the host-protocol/clock change or an explicit product
divergence, plus the matched comparison.

**B5 — worker lifetime hook and actual exit** ([#1196](https://github.com/lanshengzhi/cpp-coding-harness/issues/1196)).
Owner `cch_codemode`; public caller `CodemodeWorkerHost`. Row: `codemode-worker-lifetime`. The
close/result-before-exit observations are already compared, but the frozen throwing lifetime hook
and the Node-child timeout-exit premise have no C++ equivalent (strict no-exception build, native
pthread host). The issue requires a public failure seam or an explicit divergence and the
terminate-plus-release comparison.

## Validation

Measured on merge commit `320df81d26427a6a57f99a71e8d3b0bb0208b108` (base
`3f791c298a9628e283135862c46fd7af2b8fbc24` merged with `origin/main`
`2953743b5`, including the #1158 registration and #1182 TUI receipts) plus the selected
working-tree inputs pinned in the delivery record. The merge preserved both sides' delivery
records; only the pinned shared inputs (`baselines.json` and the consolidation plan) were
refreshed and the exact selections were rerun.

| Command | Result |
| --- | --- |
| `cmake --build --preset vcpkg --target cch_tests_codemode` | built; build-phase Parity Architecture Gate PASS |
| `cmake --build --preset vcpkg --target cch_tests_coding_agent` | built; build-phase Parity Architecture Gate PASS |
| `ctest --preset vcpkg -N -R '<52 exact codemode case names>'` | 52 selected |
| `ctest --preset vcpkg -R '<52 exact codemode case names>' --output-on-failure --output-junit docs/research/pi-v1.3.0/deliveries/1179-codemode-revalidation-main.xml` | 52 passed, 0 failed, 0 skipped on the merged tree |
| `ctest --preset vcpkg -L '^(issue1090\|issue1101\|issue1104)$' --output-junit docs/research/pi-v1.3.0/deliveries/1090-revalidation-1179m.xml` | 36 passed, 0 failed, 0 skipped (the #1090 receipt revalidated because the 1056 ledger is one of its pinned inputs) |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py build` | ledger rebuilt with 39 new delivery-backed rows |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check` | PASS — 1,464 rows, 107 delivery-backed rows, 658 scenarios, 234 explicit orphans, 0 uncovered |
| `python3 tests/evidence/ConsolidationAuditTest.py` | 42 tests, all passed |
| `python3 scripts/evidence/audit_evidence.py verify --baseline pi-v1.3.0` | 57 bundles, 118 artifacts and 661 scenarios verified at the full frozen revision |
| `ctest --preset vcpkg -L architecture` | 43 passed, 0 failed, 0 skipped |
| `scripts/format-check.sh origin/main` | clean |

The delivery selection is the exact 52-case set, not a label-wide run. The record pins the JUnit
bytes, the selected count and the case names, so the scenario mapping cannot drift silently.

No full unfiltered `ctest --preset vcpkg` run was required for this ticket by the operator; this
delivery changes tests, source-audit status facts and evidence records, not production behavior or
the build graph. The architecture selection is recorded separately because the source ledger and
the delivery record are evidence-infrastructure inputs.

The 10 remaining rows have no whole-row receipt and are not counted as verified. Their exact
owner, public caller, row/scenario IDs, normal/discriminating cases and dependency are in
B1–B5 above.

## Capability status

The 39 complete rows move to `implemented` / `verified` through the validated delivery record.
The 10 remaining rows keep their recorded implementation and verification status, their
`auditStatus` history and their named remaining work: three are implemented but source-only
(B2), three are implemented with a recorded boundary (B1/B4/B5), two are partial (B1/B4) and two
are missing (B1/B3). Membership is unchanged: 49 supported `cch_codemode` rows (plus two excluded)
before and after. The whole ledger moves from 61 to 107 delivery-backed rows after the merge
(main's #1158/#1182 receipts add seven); this is a reconciliation count, not an estimate of
completion.

No blanket promotion occurred: every delivered row maps all of its recorded comparison scenarios
and has a passing discriminating case. The excluded `models.*` provider execution and Cloudflare
Durable Object rows keep their existing boundaries and are not part of the 49-row count.

### Follow-up status and dependencies

| Batch | Issue | State | Native graph | Dependency |
| --- | --- | --- | --- | --- |
| B1 public tool table + close | [#1192](https://github.com/lanshengzhi/cpp-coding-harness/issues/1192) | open, `ready-for-agent` | sub-issue of #1183 | none before implementation; shares the close state with B3's Inline entry |
| B2 source-only captures | [#1193](https://github.com/lanshengzhi/cpp-coding-harness/issues/1193) | open, `ready-for-agent` | sub-issue of #1183 | none; independent captures from `71023aec…` |
| B3 portable no-Node entry | [#1194](https://github.com/lanshengzhi/cpp-coding-harness/issues/1194) | open, `ready-for-agent` | sub-issue of #1183 | #1192 if the portable entry exports the shared Inline/close surface |
| B4 discovery absence + host timezone | [#1195](https://github.com/lanshengzhi/cpp-coding-harness/issues/1195) | open, `ready-for-agent` | sub-issue of #1183 | owning divergence decision (ADR or explicit product choice) |
| B5 worker lifetime hook + actual exit | [#1196](https://github.com/lanshengzhi/cpp-coding-harness/issues/1196) | open, `ready-for-agent` | sub-issue of #1183 | none; requires a public failure seam or an explicit divergence |

## Remaining boundary

Batches B1–B5 are the complete remaining `cch_codemode` obligation in this packet. They must not
implement neighboring behavior, must capture any missing frozen observation independently from
`71023aec…`, and must not weaken a normalization. `#1044` and `#1126` stay open; closing this
verification packet accepts no adjacent row and no umbrella obligation.

Magpie-only AI access, XDG state roots, strict no-exception behavior, the ordinary application
Session, all explicit AI/platform exclusions and the no-historical-Pike-durable-migration boundary
remain unchanged. SIGKILL evidence does not establish power-loss or multi-process guarantees.

## Appendix — every supported `cch_codemode` row

Batches: B1 = #1192, B2 = #1193, B3 = #1194, B4 = #1195, B5 = #1196. The two excluded rows keep
their existing boundary and are not part of this table.

| Row | Capability | Disposition |
| --- | --- | --- |
| `cch_codemode:codemode-source-constants` | Codemode source grammar and options prefix | verified — whole-row receipt |
| `cch_codemode:codemode-source-parse` | Codemode source parser | verified — whole-row receipt |
| `cch_codemode:codemode-source-placement-errors` | Source placement and typed errors | verified — whole-row receipt |
| `cch_codemode:codemode-source-timeout-option` | Execution deadline | verified — whole-row receipt |
| `cch_codemode:codemode-source-output-token-option` | Application output-token budget | B2 — independent capture + comparison |
| `cch_codemode:codemode-declaration-identifier` | Identifier normalization | verified — whole-row receipt |
| `cch_codemode:codemode-declaration-input-budget` | Input-schema declaration budget | verified — whole-row receipt |
| `cch_codemode:codemode-schema-to-type` | JSON Schema to TypeScript type rendering | verified — whole-row receipt |
| `cch_codemode:codemode-render-tool-signature` | Tool declaration and output type | verified — whole-row receipt |
| `cch_codemode:codemode-script-globals` | Script globals and namespace grouping | verified — whole-row receipt |
| `cch_codemode:codemode-script-discovery` | Tool discovery globals | B4 — absence spelling |
| `cch_codemode:codemode-script-store-load` | store()/load() snapshot and write reporting | verified — whole-row receipt |
| `cch_codemode:codemode-script-output-text-image-console` | Script output channels and ordering | verified — whole-row receipt |
| `cch_codemode:codemode-script-image-format` | Image helper argument and MIME handling | verified — whole-row receipt |
| `cch_codemode:codemode-script-exit` | exit() success path | verified — whole-row receipt |
| `cch_codemode:codemode-script-error-taxonomy` | Script error names, stacks and kinds | verified — whole-row receipt |
| `cch_codemode:codemode-script-output-limits` | Prelude output and store limits | verified — whole-row receipt |
| `cch_codemode:codemode-script-unawaited-calls` | Unawaited tool calls at completion | verified — whole-row receipt |
| `cch_codemode:codemode-script-clock-timezone` | Host clock and timezone | B4 — host timezone policy |
| `cch_codemode:codemode-script-memory-limit` | VM memory limit | verified — whole-row receipt |
| `cch_codemode:codemode-script-abort-reason` | Caller cancellation reason | verified — whole-row receipt |
| `cch_codemode:codemode-execute-outcome` | Execute result shape | B1 — tool table + close surface |
| `cch_codemode:codemode-inline-close` | Inline close and execution boundary | B1 — tool table + close surface |
| `cch_codemode:codemode-inline-success` | Inline successful execution | verified — whole-row receipt |
| `cch_codemode:codemode-inline-interrupt-budget` | Inline VM interrupt budget | verified — whole-row receipt |
| `cch_codemode:codemode-inline-deadline-abort` | Inline deadline and cancellation | verified — whole-row receipt |
| `cch_codemode:codemode-remote` | Generic remote code mode execution | B2 — independent capture + comparison |
| `cch_codemode:codemode-remote-exchange` | Generic Remote exchange | verified — whole-row receipt |
| `cch_codemode:codemode-remote-output-batching` | Remote output batching | verified — whole-row receipt |
| `cch_codemode:codemode-remote-caps` | Remote interrupt/memory caps | verified — whole-row receipt |
| `cch_codemode:codemode-remote-abort-stop` | Remote abort and stop reply | verified — whole-row receipt |
| `cch_codemode:codemode-remote-failures` | Remote transport failure taxonomy | verified — whole-row receipt |
| `cch_codemode:codemode-remote-protocol` | Remote protocol data types | verified — whole-row receipt |
| `cch_codemode:codemode-remote-deterministic` | Deterministic replay over Remote | verified — whole-row receipt |
| `cch_codemode:codemode-sandbox-create-errors` | Guest module load failure | B2 — independent capture + comparison |
| `cch_codemode:codemode-portable-entry` | Portable no-Node library entry | B3 — portable target |
| `cch_codemode:codemode-worker-admission` | Worker admission and capacity | verified — whole-row receipt |
| `cch_codemode:codemode-worker-lifetime` | Result versus worker exit settlement | B5 — throwing hook + actual exit |
| `cch_codemode:codemode-worker-repeated-cancellation` | Repeated cancellation accounting | verified — whole-row receipt |
| `cch_codemode:codemode-worker-construction-failure` | Worker construction failure handling | verified — whole-row receipt |
| `cch_codemode:codemode-worker-close` | Sandbox close semantics | B1 — tool table + close surface |
| `cch_codemode:codemode-deterministic-start` | Deterministic start() and delivery | verified — whole-row receipt |
| `cch_codemode:codemode-deterministic-clock-seed` | Virtual clock and seeded randomness | verified — whole-row receipt |
| `cch_codemode:codemode-deterministic-seq` | Ordered result delivery and idle events | verified — whole-row receipt |
| `cch_codemode:codemode-deterministic-during` | during completion metadata | verified — whole-row receipt |
| `cch_codemode:codemode-deterministic-fingerprint` | VM fingerprint and replay mismatch | verified — whole-row receipt |
| `cch_codemode:codemode-deterministic-validation` | Deterministic start validation | verified — whole-row receipt |
| `cch_codemode:codemode-deterministic-properties` | Deterministic rejection properties | verified — whole-row receipt |
| `cch_codemode:codemode-deterministic-vs-execute` | Deterministic versus normal execution | verified — whole-row receipt |
