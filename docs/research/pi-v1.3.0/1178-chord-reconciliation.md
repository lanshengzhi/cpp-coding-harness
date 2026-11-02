# #1178 Chord comparison reconciliation

## Scope

Reconcile the 60 supported `cch_chord` capability rows with their current delivery evidence and
register complete whole-row comparison receipts where the full row is covered. Authority:
[#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044),
[#1183](https://github.com/lanshengzhi/cpp-coding-harness/issues/1183),
[ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md),
Named Baseline **pi-v1.3.0** at `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc.

Bounded verification scope: context cancellation/value semantics, strict JSON/output, immutable
revision/delta behavior and replicated-state publication. Document-transaction and harness-view rows
are reconciled here but their comparison receipts belong to the durable packet.

This ticket implements no neighboring feature. Membership, implementation status and verification
status stay separate: a row advances only through a complete, pinned comparison receipt.

## Reconciliation method

1. Read every supported `cch_chord` row in
   [1058-consolidated-capability-ledger.json](1058-consolidated-capability-ledger.json) and its
   recorded evidence route, scenario and gaps. The source audit facts in
   [1057-chord-durable-dependency-subset.json](1057-chord-durable-dependency-subset.json) are
   preserved as history and were not treated as current code observations.
2. For each row, find the delivered public C++ seam (`src/chord`, and the durable Session/Harness
   where the consumer lives) and the frozen artifact scenario the audit registered for it.
3. Compare the C++ case against the artifact's own observation fields. Where the delivered case did
   not yet separate every recorded field, strengthen it in place instead of claiming a mapping it did
   not have. Eight cases were extended or added for this (see §Delivered behavior).
4. A row is complete only when **every** comparison scenario in its audit record has a passing C++
   case that asserts the frozen observation fields, plus a passing discriminating case. Complete rows
   are registered as one pinned delivery record. Every other row keeps its recorded status and names
   a bounded follow-up (batch issue or routed packet).

## Delivered behavior

### Complete delivered rows (23)

One delivery record, `1178-chord-comparisons`, in
[1058-deliveries.json](1058-deliveries.json), pins the production sources, the comparison test
sources, the frozen artifacts and manifests, the evidence tooling, the CTest JUnit result and the
resolved scenario routing. Each row's `remainingBoundary` is empty: nothing is deferred.

| Row | Frozen artifact | Scenarios compared | C++ case(s) |
| --- | --- | ---: | --- |
| `cch_chord:context:independent-cancel` | `audit-1057/context-cancellation.json` | 1 | `a child cancellation is isolated while parent cancellation reaches an already-created context` (`tests/chord/ChordContextTest.cpp`) |
| `cch_chord:context:parent-cancellation` | `audit-1057/context-cancellation.json` | 1 | `a child cancellation is isolated while parent cancellation reaches an already-created context` (`tests/chord/ChordContextTest.cpp`) |
| `cch_chord:context:value-layering` | `audit-1057/context-cancellation.json` | 1 | `a child cancellation is isolated while parent cancellation reaches an already-created context` (`tests/chord/ChordContextTest.cpp`) |
| `cch_chord:context:with-abort-signal` | `audit-1057/context-cancellation.json` | 1 | `a child cancellation is isolated while parent cancellation reaches an already-created context` (`tests/chord/ChordContextTest.cpp`) |
| `cch_chord:context:without-abort-signal` | `audit-1057/context-cancellation.json` | 1 | `without abort signal masks cancellation while preserving typed values` (`tests/chord/ChordContextTest.cpp`) |
| `cch_chord:delta:batch-application` | `audit-1057/delta-revisions.json` | 1 | `ordered immutable batches produce one final revision without changing the base` (`tests/chord/DeltaTest.cpp`) |
| `cch_chord:delta:draft-abort` | `audit-1057/delta-revisions.json` | 1 | `aborting a prepared change revokes escaped draft handles` (`tests/chord/TrackerTest.cpp`) |
| `cch_chord:delta:immutable-application` | `audit-1057/delta-revisions.json` | 1 | `immutable application reproduces a revision while sharing untouched containers` (`tests/chord/DeltaTest.cpp`) |
| `cch_chord:delta:mutable-application` | `audit-1061/validation-order.json` | 1 | `application follows frozen sequential validation and replacement semantics` (`tests/chord/DeltaTest.cpp`) |
| `cch_chord:delta:op-vocabulary` | `audit-1057/delta-revisions.json` | 1 | `tracker drafts emit the frozen set append splice permutation delete truncate and replace vocabulary` (`tests/chord/DeltaTest.cpp`) |
| `cch_chord:delta:overlap` | `audit-1057/delta-revisions.json` | 1 | `overlap returns the bounded common run` (`tests/chord/DeltaTest.cpp`) |
| `cch_chord:delta:prepare-replace` | `audit-1061/prepare-replace.json` | 3 | `an aborted or foreign whole-root preparation cannot be adopted`<br>`prepare replace keeps the checkpoint base immutable and stales same-base preparations`<br>`prepare replace of a deeply equal root emits no operations and stales a competitor` (`tests/chord/TrackerTest.cpp`) |
| `cch_chord:delta:stale-preparation` | `audit-1057/delta-revisions.json` | 1 | `a same-base preparation becomes stale after another preparation is adopted` (`tests/chord/TrackerTest.cpp`) |
| `cch_chord:delta:tracker-revision` | `audit-1057/delta-revisions.json` | 1 | `a same-base preparation becomes stale after another preparation is adopted` (`tests/chord/TrackerTest.cpp`) |
| `cch_chord:output:bound-output` | `audit-1057/json-output-support.json` | 1 | `bound output keeps the configured head or tail and reports exact dropped counts` (`tests/chord/OutputTest.cpp`) |
| `cch_chord:output:character-end` | `audit-1057/json-output-support.json` | 1 | `UTF-8 byte length and character-end boundaries match the frozen output helpers` (`tests/chord/OutputTest.cpp`) |
| `cch_chord:output:output-buffer` | `audit-1057/json-output-support.json` | 1 | `output buffer retains the frozen tail window and exact whole-stream totals` (`tests/chord/OutputTest.cpp`) |
| `cch_chord:output:utf8-byte-length` | `audit-1057/json-output-support.json` | 1 | `UTF-8 byte length and character-end boundaries match the frozen output helpers` (`tests/chord/OutputTest.cpp`) |
| `cch_chord:state:attached-hydration` | `audit-1057/replicated-state.json`, `audit-1062/ordered-frames.json`, `audit-1062/review-edges.json` | 6 | `a retained value reference keeps the revision it was read at`<br>`attached replicated state hydrates at cursor zero and then delivers exact frames in sequence order`<br>`attached state restores the latest committed revision when subscribing after existing commits`<br>`attachment captures the snapshot before activation and drains buffered frames in commit order`<br>`exact source values are published without applying or re-diffing frame operations`<br>`negative safe integer cursors are accepted for the snapshot and later frames` (`tests/chord/ReplicatedStateTest.cpp`) |
| `cch_chord:state:cursor-gap` | `audit-1057/replicated-state.json`, `audit-1062/ordered-frames.json`, `audit-1062/review-edges.json` | 5 | `a cursor gap is reported once and leaves the last good replicated value`<br>`a frame cursor outside the safe integer range fails the attachment and keeps the last good value`<br>`a gap while draining buffered activation stops dispatch without touching a disposed listener`<br>`a repeated cursor is reported once as a gap and keeps the last good value`<br>`a source gap without an onError hook writes the exact default diagnostic once and keeps the last good value` (`tests/chord/ReplicatedStateTest.cpp`) |
| `cch_chord:state:dispose` | `audit-1057/replicated-state.json`, `audit-1062/observer-delivery.json`, `audit-1062/ordered-frames.json`, `audit-1062/review-edges.json` | 4 | `a snapshot cursor outside the safe integer range fails attach and disposes the attachment once`<br>`attached replicated state hydrates at cursor zero and then delivers exact frames in sequence order`<br>`independent attachments converge separately and dispose idempotently`<br>`unsubscribe discards queued deliveries without aborting the running callback` (`tests/chord/ReplicatedStateTest.cpp`) |
| `cch_chord:state:listener-error` | `audit-1057/replicated-state.json`, `audit-1062/observer-delivery.json` | 4 | `a callback rejection that settles after unsubscribe is still reported`<br>`a failing listener is reported in isolation and keeps receiving later deliveries`<br>`a failing state listener is reported through onError without stopping another subscription`<br>`a synchronous hydration failure is reported without removing the subscription` (`tests/chord/ReplicatedStateTest.cpp`) |
| `cch_chord:state:pending-bound` | `audit-1062/observer-delivery.json` | 4 | `at most 100 deliveries pend behind one held listener while a commit fits the bound`<br>`overflow at the 100-pending bound keeps the newest pending delivery`<br>`overflow keeps only the newest pending delivery with its exact value context and delivery`<br>`repeated overflow at the 100-pending bound keeps the newest pending delivery` (`tests/chord/ReplicatedStateTest.cpp`) |

The comparison cases are the delivered #1060/#1061/#1062 public-seam tests. This reconciliation
strengthened the ones that did not yet separate every frozen field:

| Case | Change | Frozen fields now compared |
| --- | --- | --- |
| `a child cancellation is isolated while parent cancellation reaches an already-created context` | added assertions | root/child/sibling/inheritor abort-signal presence, sibling value, ordered observations |
| `without abort signal masks cancellation while preserving typed values` | added assertions | plain background and un-aborted external signal presence |
| `immutable application reproduces a revision while sharing untouched containers` | added assertion | the full `nextAgain` revision value |
| `application follows frozen sequential validation and replacement semantics` | added case | `a later empty path keeps the preceding mutable write and reports the empty path` |
| `attached replicated state hydrates at cursor zero and then delivers exact frames in sequence order` | new case | both deliveries' kind/sequence/value, absent abort signal, no errors, one disposal |
| `overflow keeps only the newest pending delivery with its exact value context and delivery` | rewritten context observation | the newest pending context's abort-signal presence, and the adopted revision at overflow |
| `a retained value reference keeps the revision it was read at` | added assertion | the retained revision shares the committed revision's container |
| `a failing state listener is reported through onError without stopping another subscription` | new case | the `audit-1057` listener-failure isolation scenario |

`cch_chord:output:bound-output` and `cch_chord:output:output-buffer` share one frozen scenario:
`bound output keeps the configured head or tail and reports exact dropped counts` covers its
`boundOutput` half and `output buffer retains the frozen tail window and exact whole-stream totals`
covers its `OutputBuffer` half, both in the pinned `tests/chord/OutputTest.cpp` and both inside the
pinned selection.

### Additional registered observations consumed by the same pinned cases

Six registered `audit-1061` scenarios are not any row's `comparisonScenarios` (the receipt contract
binds exactly a row's recorded obligations), and the consolidation plan keeps them as explicit
orphans with a disposition that the Chord delta slices must consume them. Every one of them is
compared by a passing case in the pinned selection, so none is unassigned:

| Orphan scenario | Pinned case |
| --- | --- |
| `deleting an array element from a draft rejects the hole and leaves the prepared revision empty` (`draft-array-holes.json`) | `draft paths reject unsafe empty and out-of-range mutations before recording` |
| `a later traversal failure keeps the preceding mutable write` (`review-fixes.json`) | `application follows frozen sequential validation and replacement semantics` |
| `draft reads distinguish absent leaves from null and from absent receiver paths` (`review-fixes.json`) | `draft reads distinguish absent leaves nulls and invalid traversal` |
| `adoption immediately revokes competing open drafts while prepared competitors retain normal prepared abort and stale states` (`review-fixes.json`) | `adoption immediately revokes escaped competing draft handles` |
| `adding a key and erasing it again normalizes to an empty batch over the unchanged base` (`review-fixes.json`) | `erasing a newly added key prepares an empty batch over the unchanged base` |
| `an existing nested key keeps explicit delete and readd order` (`review-fixes.json`) | `draft writes fold to the final leaf while delete and readd stays explicit` |

### Correction: three inert scenario links

Three `scenarioLinks` entries in
[consolidation-plan.json](../../../fixtures/pi-v1.3.0/consolidation-1058/consolidation-plan.json)
named the consolidated `cch_chord:delta:prepare-replace` row but carried `"ticket": "1061"`. The
lane filter matches a link's ticket against the source lane, and no `1061` source lane exists, so
the three links never applied: the row's recorded obligations silently omitted the deeply-equal and
aborted whole-root preparation scenarios. This reconciliation corrected the ticket to `1057` (the
lane that owns the row), so all three `audit-1061/prepare-replace.json` scenarios are now the row's
recorded obligations and all three are compared by pinned cases. The change is recorded as
`revalidation1178` on the two records that pin the plan (#1101 and #1118); neither of those records
changes its rows, scenarios or routing digest, and no new verification claim is made about them.

No production behavior changed. A comparison that fails is a finding about the implementation, not a
test to relax, so no normalization was added and no expectation was derived from C++.

### Remaining obligations (37 rows)

Every remaining row names a bounded batch. No row is left to
[#1126](https://github.com/lanshengzhi/cpp-coding-harness/issues/1126), and no row moved to Deferred.

**B1 — context wait and observer cancellation**
([#1184](https://github.com/lanshengzhi/cpp-coding-harness/issues/1184)). Owner `cch_chord`; public
caller `Context`/`with_cancel`/`without_abort_signal` plus the durable `Session` watch/close.
Rows: `context:await-with-context`, `context:watch-cancellation`, `context:close-cancellation`,
`context:acquire-cancellation`, `context:root-background`, `Chord:context`. Scenarios: the four
remaining `audit-1057/context-cancellation.json` scenarios, the missing root-context capture, and the
`1054` composite declaration. Discriminating cases: a waiter that cancels its producer, a watch stop
that aborts a running callback, a cancelled close that skips storage cleanup, and an acquisition that
leaves a staged observer.

**B2 — delta ingestion and applier identity**
([#1185](https://github.com/lanshengzhi/cpp-coding-harness/issues/1185)). Owner `cch_chord`; public
caller `apply`/`apply_immutable`/`apply_immutable_batches`/`validate_batch`/`Tracker`. Rows:
`delta:op-validation`, `delta:path-validation`, `delta:root-replacement`, `Chord:delta`,
`Chord:draft`. Blockers: two of the three recorded rejections (`unknown op verb: x`, `p index`) have
no spelling in the typed `Op` variant, and the frozen root-replacement identity observations
(`sameTarget`, `replacementIsNew`) have no spelling with the out-parameter applier.

**B3 — typed JSON ingestion**
([#1186](https://github.com/lanshengzhi/cpp-coding-harness/issues/1186)). Owner `cch_chord`; public
caller `copy_strict_json`/`is_strict_json_value` and the durable value-admission boundary. Rows:
`json:copy-json`, `json:strict-validation`, `json:json-value`, `json:json-representation`,
`Chord:json`. Blocker: `support::JsonValue` cannot represent `undefined`, functions, class instances
or sparse arrays, so the frozen rejection vocabulary and the omit-undefined policy have no carrier
spelling.

**B4 — progress pacing and rate**
([#1187](https://github.com/lanshengzhi/cpp-coding-harness/issues/1187)). Owner `cch_chord`; public
caller the output helpers next to `bound_output`/`OutputBuffer`. Rows: `output:progress`,
`output:progress-rate`. Blocker: no `PROGRESS_BYTES_PER_SECOND` constant and no pacing seam; the
audit deliberately left pacing unverified rather than normalizing timing away.

**B5 — observer serialization across the durable watch seam**
([#1188](https://github.com/lanshengzhi/cpp-coding-harness/issues/1188)). Owner `cch_chord`; public
caller `AttachedReplicatedState::subscribe` plus the durable `DocumentWatch`. Rows:
`state:delivery-order`, `Chord:state`. Blocker: five of six scenarios pass; the remaining
`a watch acquired before start buffers frames and serializes callbacks` observes an awaited listener
(`maxConcurrentCallbacks`), which neither delivered listener seam can hold.

**B6 — durable-owned rows** (routed to
[#1180](https://github.com/lanshengzhi/cpp-coding-harness/issues/1180#issuecomment-6551377403)). Rows:
the fifteen `document:*` rows and `chord-delta`, `chord-documents`. Their only public comparison seam
is the durable Session/Harness delivered by #1063; the durable packet owns those receipts.

## Validation

Measured on this ticket's comparison commit `a3f9e28af` (checkout `a3f9e28af` plus the receipt and
report commit):

| Command | Result |
| --- | --- |
| `cmake --build --preset vcpkg --target cch_tests_chord` | built; build-phase Parity Architecture Gate PASS |
| `ctest --preset vcpkg -N -L chord` | 70 selected |
| `ctest --preset vcpkg -L chord --output-on-failure --output-junit docs/research/pi-v1.3.0/deliveries/1178-chord.xml` | 70/70 passed, 0 failed, 0 skipped |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py build` | ledger rebuilt; 34 delivery-backed rows (23 from this record) |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check` | PASS — 562 scenarios, 362 named by a row, 56 covered by a referenced artifact, 144 explicit orphans, 0 uncovered |
| `python3 tests/evidence/ConsolidationAuditTest.py` | 42 tests, all passed |
| `python3 scripts/evidence/audit_evidence.py verify --baseline pi-v1.3.0` | 46 bundles, 102 artifacts and 565 scenarios verified at `71023aec…` |
| `cmake --build --preset vcpkg` then `ctest --preset vcpkg` | 4,104 selected, 4,087 passed, 9 failed, 8 skipped (details below) |
| `scripts/format-check.sh 0b118e912` | clean |

[#1185](1185-chord-delta-ingestion.md) later changed `src/chord/Delta.cpp`, its header and
`tests/chord/DeltaTest.cpp` while adding the decoded-operation ingestion seam. Three inputs this
receipt pins therefore changed, and the selection now has six more cases. The shared selection was
rerun on the #1185 merged tree into `deliveries/1178-revalidation-1185.xml` (76 selected, 76 passed,
including every case this record claims) and the record carries a `revalidation1185` entry with the
refreshed digests. No claimed row, scenario or routing digest changed, and the original
`deliveries/1178-chord.xml` stays committed.

The receipt pins the exact JUnit bytes, so the selected count, the case names and the scenario
mapping cannot drift silently.

The unfiltered full run on this host reported nine failures, none in a `cch_chord` case:

- `Projection publishes 100 message-update chunks inside the issue cost bound` is one of the two
  documented #632 quarantine performance bounds; it fails on this loaded runner and passes under the
  recorded re-enable condition.
- `real PTY records Herdr markdown and OSC 52 terminal bytes` requires a Herdr-hosted PTY
  (`TERM_PROGRAM=herdr`); this host reports `ghostty`. It is an environment prerequisite, not a code
  observation.
- `Native TUI fullscreen scroll-to-end indicator paints on a direct-fullscreen launch` fails
  identically on the branch base `0b118e912` in the primary checkout, so it is a pre-existing
  failure unrelated to this ticket.
- `Durable codemode replays committed answers after SIGKILL` (timeout), `Process Terminal atomic stop
  and default stop detect postdrain EIO`, `a timed-out query resolves partial results and later
  replies advance state`, `a public codemode caller stops pure computation with the interrupt budget`,
  `codemode worker results settle before exit while admission stays bounded` and `MCP boot warning is
  surfaced when codemode auto-enable is disabled` each passed when re-run in isolation on the same
  tree; the unfiltered run was load-limited (two full runs on this host failed different sets, both
  outside `cch_chord`).

No assertion, timeout or label exclusion was changed to obtain that result, and the eight skips
supply no acceptance evidence.

## Capability status

Exactly 23 rows move from their recorded audit status to `implemented` / `verified`, each through the
validated delivery record. All other chord rows keep their recorded implementation and verification
status, their `auditStatus` history and their named remaining work. Membership is unchanged: 60
supported and 5 excluded `cch_chord` rows before and after. The excluded plugin/host, RPC-link,
cluster, cell/schema and Node-host surfaces stay excluded.

Eleven rows were verified across the whole ledger before this pass; this pass adds 23 for 34 total and
promotes no other owner's row. This is a reconciliation count, not a completion percentage.

## Remaining boundary

Batches B1–B7 above are the complete remaining chord obligation. They must not implement neighboring
behavior, must capture any missing frozen observation instead of deriving it from C++, and must not
weaken a normalization. `#1044` and `#1126` stay open; closing this verification packet accepts no
adjacent row and no umbrella obligation.

Magpie-only AI access, XDG state roots, strict no-exception behavior, the ordinary application
Session, all explicit AI/platform exclusions and the no-historical-Pike-durable-migration boundary
remain unchanged. SIGKILL evidence does not establish power-loss or multi-process guarantees.

## Appendix — every supported `cch_chord` row

Batches: B1 = #1184, B2 = #1185, B3 = #1186, B4 = #1187, B5 = #1188, B6/B7 = the durable packet
#1180. Excluded rows keep their existing boundary and are not part of this reconciliation.

| Row | Capability | Disposition |
| --- | --- | --- |
| `cch_chord:Chord:context` | Context values, abort signals, cancellation and background context | B1 — implementation/capture |
| `cch_chord:Chord:delta` | Chord delta operations and apply semantics | B2 — comparison seam |
| `cch_chord:Chord:draft` | Draft tracking, operation batches and escaped-draft revocation | B2 — comparison seam |
| `cch_chord:Chord:json` | Strict JSON values and immutable ownership transfer | B3 — typed JSON boundary |
| `cch_chord:Chord:state` | Attached replicated state with immutable values, delivery and dispose | B5 — durable watch serialization |
| `cch_chord:chord-delta` | Context delta/patch observation | B7 — durable packet (#1180) |
| `cch_chord:chord-documents` | Durable-required Chord document/value semantics | B7 — durable packet (#1180) |
| `cch_chord:context:acquire-cancellation` | Cancelled watch acquisition | B1 — implementation/capture |
| `cch_chord:context:await-with-context` | Cancellation-aware wait | B1 — implementation/capture |
| `cch_chord:context:close-cancellation` | Cancelled close still performs cleanup | B1 — implementation/capture |
| `cch_chord:context:independent-cancel` | Independent cancellation of a child context | **verified** — whole-row receipt |
| `cch_chord:context:parent-cancellation` | Parent-to-child cancellation propagation | **verified** — whole-row receipt |
| `cch_chord:context:root-background` | Empty and background root Context values | B1 — implementation/capture |
| `cch_chord:context:value-layering` | Typed value layering and lookup | **verified** — whole-row receipt |
| `cch_chord:context:watch-cancellation` | Cancellation of an observation wait | B1 — implementation/capture |
| `cch_chord:context:with-abort-signal` | Combining an external abort signal with its parent | **verified** — whole-row receipt |
| `cch_chord:context:without-abort-signal` | Detached cleanup context | **verified** — whole-row receipt |
| `cch_chord:delta:batch-application` | Ordered batch application | **verified** — whole-row receipt |
| `cch_chord:delta:draft-abort` | Draft abort and handle revocation | **verified** — whole-row receipt |
| `cch_chord:delta:immutable-application` | Copy-on-write immutable application | **verified** — whole-row receipt |
| `cch_chord:delta:mutable-application` | Materializing mutable application | **verified** — whole-row receipt |
| `cch_chord:delta:op-validation` | Operation shape and index validation | B2 — comparison seam |
| `cch_chord:delta:op-vocabulary` | Delta operation vocabulary | **verified** — whole-row receipt |
| `cch_chord:delta:overlap` | Bounded overlap search | **verified** — whole-row receipt |
| `cch_chord:delta:path-validation` | Unsafe path rejection | B2 — comparison seam |
| `cch_chord:delta:prepare-replace` | Prepared full replacement | **verified** — whole-row receipt |
| `cch_chord:delta:root-replacement` | Root replacement and base detection | B2 — comparison seam |
| `cch_chord:delta:stale-preparation` | Stale preparation rejection | **verified** — whole-row receipt |
| `cch_chord:delta:tracker-revision` | Monotonic tracked revisions | **verified** — whole-row receipt |
| `cch_chord:document:base-creation` | Creation and migration base publication | B6 — durable packet (#1180) |
| `cch_chord:document:callback-rollback` | Callback rollback | B6 — durable packet (#1180) |
| `cch_chord:document:checkpoint-bases` | Checkpoint base selection | B6 — durable packet (#1180) |
| `cch_chord:document:commit-publication` | Ordered committed document publications | B6 — durable packet (#1180) |
| `cch_chord:document:draft-lifecycle` | Typed document draft lifecycle | B6 — durable packet (#1180) |
| `cch_chord:document:immutable-snapshots` | Immutable historical snapshots | B6 — durable packet (#1180) |
| `cch_chord:document:retention` | Document retention and eviction | B6 — durable packet (#1180) |
| `cch_chord:document:retirement` | Document retirement | B6 — durable packet (#1180) |
| `cch_chord:document:revision-mismatch` | Revision/definition mismatch diagnostics | B6 — durable packet (#1180) |
| `cch_chord:document:state-delivery` | Typed document state hydration and updates | B6 — durable packet (#1180) |
| `cch_chord:document:storage-failure` | Fatal storage failure | B6 — durable packet (#1180) |
| `cch_chord:document:structural-no-op` | Structural no-op publication | B6 — durable packet (#1180) |
| `cch_chord:document:watch-acquisition-cancellation` | Cancellable watcher acquisition | B6 — durable packet (#1180) |
| `cch_chord:document:watch-buffer` | Watcher pre-start buffering and serialization | B6 — durable packet (#1180) |
| `cch_chord:document:watch-overflow` | Watcher pending-frame overflow | B6 — durable packet (#1180) |
| `cch_chord:excluded:cells-schema` | Chord cells/schema tooling | excluded (unchanged) |
| `cch_chord:excluded:cluster` | Chord cluster placement/readiness | excluded (unchanged) |
| `cch_chord:excluded:node-host` | Chord Node host adapter | excluded (unchanged) |
| `cch_chord:excluded:plugin-host` | Chord plugin/host runtime | excluded (unchanged) |
| `cch_chord:excluded:rpc-links` | Chord links/RPC transport | excluded (unchanged) |
| `cch_chord:json:copy-json` | Strict detached JSON copying | B3 — typed JSON boundary |
| `cch_chord:json:json-representation` | Compile-time JSON representation mapping | B3 — typed JSON boundary |
| `cch_chord:json:json-value` | JSON value type | B3 — typed JSON boundary |
| `cch_chord:json:strict-validation` | Strict JSON validation | B3 — typed JSON boundary |
| `cch_chord:output:bound-output` | Head/tail output bounding | **verified** — whole-row receipt |
| `cch_chord:output:character-end` | UTF-8 character boundary | **verified** — whole-row receipt |
| `cch_chord:output:output-buffer` | Incremental bounded output buffer | **verified** — whole-row receipt |
| `cch_chord:output:progress` | Throttled progress publication | B4 — progress |
| `cch_chord:output:progress-rate` | Progress byte-rate constant | B4 — progress |
| `cch_chord:output:utf8-byte-length` | UTF-8 byte length | **verified** — whole-row receipt |
| `cch_chord:state:attached-hydration` | Attached replicated state hydration | **verified** — whole-row receipt |
| `cch_chord:state:cursor-gap` | Replicated source cursor-gap detection | **verified** — whole-row receipt |
| `cch_chord:state:delivery-order` | Per-subscriber delivery serialization | B5 — durable watch serialization |
| `cch_chord:state:dispose` | Idempotent state disposal | **verified** — whole-row receipt |
| `cch_chord:state:listener-error` | Observation listener failure isolation | **verified** — whole-row receipt |
| `cch_chord:state:pending-bound` | Bounded pending observation | **verified** — whole-row receipt |
