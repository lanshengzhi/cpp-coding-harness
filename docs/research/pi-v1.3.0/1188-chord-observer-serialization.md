# #1188 Chord observer serialization across the durable watch seam

## Scope

Close the last [#1178](https://github.com/lanshengzhi/cpp-coding-harness/issues/1178) follow-up: the
one `cch_chord:state:delivery-order` comparison scenario that stayed blocked and the
`cch_chord:Chord:state` composite declaration. Authority:
[#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044),
[#1183](https://github.com/lanshengzhi/cpp-coding-harness/issues/1183),
[ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md),
Named Baseline **pi-v1.3.0** at `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc.

Bounded scope: the Chord replicated-state observer comparison and one reviewed coarse-row
re-pairing. `src/durable` is untouched: the durable `DocumentWatch` observation seam delivered by
#1063/#1184 keeps its synchronous listener, its acquisition revision and its buffered-frame drain.
No document transaction, delta/revision, storage, Harness or Session behavior changes.

## Seam decision

The frozen scenario is a composite of two seams:

- `packages/durable/src/session/observation.ts:CommittedWatch` owns the **acquisition** half — the
  watch holds its acquisition revision, buffers committed frames until `start()`, and then delivers
  them in commit order.
- `packages/chord/src/state.ts:StateSubscriber` owns the **serialization** half — a subscription
  runs one callback at a time and buffers later deliveries behind it.

In C++ the durable `DocumentFrameListener` is a synchronous `void` callback
(`src/durable/include/cch/durable/Session.hpp`), so no delivery there can be held across an await;
the frozen `valuesBeforeRelease` / `maxConcurrentCallbacks` observation is structurally
unrepresentable in that seam without adding a second observer runtime. The issue's option (a) — a
durable `CommittedWatch` async-listener bridge with a pending bound and retirement batch — would
either change the delivered #1184 `DocumentWatch` listener contract or add a durable
`ReplicatedStateSource` adapter, and it is not needed: the Chord `ReplicatedStateSink` is already
`support::AsyncResult`-based, and `StateSubscriber` already implements the whole serialized-domain
contract (one delivery in flight per subscription, `kReplicatedStatePendingLimit` buffered behind
it, overflow coalesced to the newest pending delivery).

This slice therefore takes the issue's option (b) as a reviewed statement rather than a new bridge:
**the delivered Chord serialized-domain contract already separates the concurrency, and the frozen
expectation fields are compared through it.** No production behavior changed.

### Seam mapping and the one normalization

The frozen watch installs its listener at `start()`, so its delivered sequence begins at the first
frame committed after the acquisition revision; Chord installs the listener at `subscribe()`, so the
acquisition revision itself arrives as the cold hydration and the frozen frame sequence is the
subscription's **update** sequence. Everything else is compared exactly:

| Frozen field (artifact `expected`) | C++ comparison |
| --- | --- |
| `valuesBeforeRelease` `[1]` | Updates recorded after the first commit and the second commit while the first delivery is held |
| `valuesAfterRelease` `[1,2]` | Updates recorded after the held delivery settles |
| `maxConcurrentCallbacks` `1` | Entry/exit counter around the listener, decremented when the delivery's `AsyncCompletion` settles |
| `finalValue` `{value: 2}` | `AttachedReplicatedState::value()` equals `revision(2)` |

The cold hydration is asserted separately (`[0]`), so the acquisition delivery cannot be silently
folded into the frozen frame sequence. The frozen `inputs.document` and `inputs.commitsBeforeStart`
describe the durable watch's acquisition/start schedule, not an expectation; the C++ case keeps the
acquisition-before-frames order and records the one spelling difference instead of normalizing a
frozen expectation. The frozen pre-start buffer itself has a delivered C++ spelling as well: the
Chord attachment captures its snapshot before activation and drains every frame buffered in that
window in commit order (`attachment captures the snapshot before activation and drains buffered
frames in commit order`, already compared under `state:attached-hydration`). It is cited here rather
than re-claimed, because this row's obligation is the observer serialization.

## Delivered behavior

### Complete delivered rows (2)

One delivery record, `1188-chord-observer-serialization`, in
[1058-deliveries.json](1058-deliveries.json), pins the production sources, the comparison test
sources, the frozen artifacts and manifests, the evidence tooling, both this report and the CTest
JUnit result. Each row's `remainingBoundary` is empty.

| Row | Frozen artifact and scenario | Comparison case |
| --- | --- | --- |
| `cch_chord:state:delivery-order` | `audit-1057/replicated-state.json`: `a watch acquired before start buffers frames and serializes callbacks` | `a watch acquired before start buffers frames and serializes callbacks` (`tests/chord/ReplicatedStateTest.cpp`) |
| | `audit-1062/ordered-frames.json`: `reentrant commits buffer in order and a late subscriber hydrates past them at the latest sequence` | `reentrant commits buffer in order and a late subscriber hydrates at the latest sequence` (`tests/chord/ReplicatedStateTest.cpp`) |
| | `audit-1062/ordered-frames.json`: `one subscription serializes reentrant hydration and update callbacks in publication order` | `one subscription serializes reentrant hydration and update callbacks in publication order` (`tests/chord/ReplicatedStateTest.cpp`) |
| | `audit-1062/observer-delivery.json`: `a held listener serializes its own updates while another subscription converges independently` | `a held listener serializes its own updates while another subscription converges independently` (`tests/chord/ReplicatedStateTest.cpp`) |
| | `audit-1062/observer-delivery.json`: `two subscriptions of the same listener are delivered independently` | `two subscriptions of the same listener behavior are delivered independently` (`tests/chord/ReplicatedStateTest.cpp`) |
| | `audit-1062/review-edges.json`: `subscribers are delivered in registration order and a reentrant subscriber hydrates before the next publication` | `subscribers are delivered in registration order with reentrant unsubscribe and subscribe` (`tests/chord/ReplicatedStateTest.cpp`) |
| `cch_chord:Chord:state` | `audit-1057/replicated-state.json`: `attached replicated state hydrates at cursor zero and then delivers exact frames in sequence order` (reviewed re-pairing, below) | `attached replicated state hydrates at cursor zero and then delivers exact frames in sequence order` (`tests/chord/ReplicatedStateTest.cpp`) |

Five of the six `state:delivery-order` scenarios were already compared by the delivered #1062 cases
and keep their exact comparison test names. Only the audit-1057 watch scenario needed a case.

### Discriminating cases

| Row | Discriminating case | Why the comparison alone cannot separate it |
| --- | --- | --- |
| `state:delivery-order` | `a watch acquired before start buffers frames and serializes callbacks` | The case is also the discriminator for the one scenario that had none: it requires the second committed frame to wait behind the held delivery and then arrive. Two throwaway probes were run on the delivered tree before this record was written. A drain loop that ignored the pending handshake delivered both callbacks concurrently (`max_active` 2, updates `[1,2]` before the release) and failed; a `push` that dropped frames while a callback was running left the updates at `[1]` after the release and failed. |
| `Chord:state` | `exact source values are published without applying or re-diffing frame operations` | The composite capability is "immutable values, delivery and dispose". A state that re-derived the published revision from the frame's operation batch would publish `7` where the committed frame carries `1`, and the case separates that without the frozen hydration sequence needing an operation batch. |

The two other frozen discriminators the issue names are the reentrant-listener cases
(`one subscription serializes reentrant hydration and update callbacks in publication order`,
`reentrant commits buffer in order and a late subscriber hydrates past them at the latest
sequence`) and the held-listener isolation case
(`a held listener serializes its own updates while another subscription converges independently`),
all pinned by the same record. A publisher gated by the held subscription would not show the other
subscriber at `1` and `2` before the held deliveries settle.

### Reviewed re-pairing of the coarse 1054 row

`cch_chord:Chord:state` is the 1054 composite view of the 1057 detail rows. Its source row
declared the placeholder scenario `document watches and states deliver only committed frames` with
the artifact `audit-1054/session-transactions.json`, whose observation covers the durable watch
frames, the Session close reason and the document state together. The row's own capability,
source endpoint (`packages/chord/src/state.ts:replicatedState,AttachedReplicatedState`) and stale
gap ("No C++ replicated state exists; owned by #1057") are the Chord state alone; the delivered
comparison seam is the Chord state.

The source row is re-paired, in
[1054-durable-session-documents-storage.json](1054-durable-session-documents-storage.json), to the
registered `audit-1057/replicated-state.json` scenario that observes exactly the row's capability:
the state hydrates at cursor zero, delivers update sequence 1 with the exact committed value,
reports no listener errors and disposes its attachment once. This is the same reviewed correction
#1186 applied to the coarse `cch_durable:Chord:json` row: one coarse row re-paired to a registered
detail scenario of its own capability, with no derived C++ expectation. The durable watch half of
the old placeholder stays with the durable document rows (the durable packet, #1180); it is not a
Chord state obligation.

### Reviewed statement: no durable async-listener bridge

The issue's option (a) is deliberately not built:

- The durable `DocumentWatch` listener is the frozen `void` callback shape. Changing it to
  `AsyncResult` would change the observation contract #1184 delivered and compared
  (`DocumentFrameListener` carries `{frame, commit context}`), for no new frozen observation.
- A durable `ReplicatedStateSource` adapter over `CommittedWatch` would be a second observer
  runtime behind the Chord state, duplicating delivery, pending-bound and error semantics that
  `StateSubscriber` already owns; the issue asks not to add one.
- Every frozen expectation field in this scenario is a Chord-state serialization fact, and the
  acquisition/start half is the durable watch's own already-delivered behavior. The comparison is
  therefore registered through the existing seam instead of widening either package.

`cch_chord:document:watch-buffer` and `cch_chord:document:watch-overflow` keep their recorded status:
they are the durable watch rows routed to the durable packet (#1180), which owns the durable
`CommittedWatch` receipts. This slice makes no claim about them.

## Validation

Measured on this ticket's implementation commit `10671a181` (checkout `10671a181` plus the receipt
and report commit):

| Command | Result |
| --- | --- |
| `cmake --build --preset vcpkg` | built; build-phase Parity Architecture Gate PASS |
| `ctest --preset vcpkg -N -L issue1188` | 1 selected |
| `ctest --preset vcpkg -L issue1188 --output-on-failure` | 1/1 passed, 0 failed, 0 skipped |
| `ctest --preset vcpkg -N -L chord` | 85 selected |
| `ctest --preset vcpkg -L chord --output-on-failure --output-junit docs/research/pi-v1.3.0/deliveries/1188-chord.xml` | 85/85 passed, 0 failed, 0 skipped |
| `ctest --preset vcpkg -L chord --output-on-failure --output-junit docs/research/pi-v1.3.0/deliveries/1178-revalidation-1188.xml` | 85/85 passed, 0 failed, 0 skipped |
| `ctest --preset vcpkg -L architecture` | 43/43 passed |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py build` | ledger rebuilt; 60 delivery-backed rows (2 from this record) |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check` | PASS |
| `python3 tests/evidence/ConsolidationAuditTest.py` | 42 tests, all passed |
| `python3 scripts/evidence/audit_evidence.py verify --baseline pi-v1.3.0` | 55 bundles, 115 artifacts and 652 scenarios verified at `71023aec…` |
| `TERM_PROGRAM=herdr ctest --preset vcpkg -j1` | 4,239 selected, 4,227 passed, 1 failed, 11 skipped (details below) |
| `scripts/format-check.sh f492ca41e` | clean |

No `cch_chord` case failed. The one failure was
`an MCP call errors explicitly when the server dies between tools/list and tools/call`
(`tests/coding_agent/McpFailureIsolationTest.cpp`, owner #872): a process-death race in the MCP
failure-isolation test double. It failed once under the serial full run and passed 2 of 3 isolated
reruns on the same tree. It is outside this slice, no MCP or durable watch source changed here, and
it is recorded instead of quarantined or retuned. The 11 skips are environment-gated pi-checkout
and dual-runtime differential cases; they supply no acceptance evidence. No assertion, timeout or
label exclusion was changed to obtain the result.

## Capability status

Two rows move from their recorded audit status to `implemented` / `verified`, each through the
validated delivery record:

- `cch_chord:state:delivery-order` (from `implemented` / `upstream-captured`)
- `cch_chord:Chord:state` (from `missing` / `upstream-captured`, through the reviewed re-pairing)

All other Chord rows keep their recorded implementation and verification status, their
`auditStatus` history and their named remaining work. Membership is unchanged: no row is added,
removed or re-owned, and no neighboring owner's row is promoted. The ledger moves from 58 to 60
delivery-backed rows.

## Remaining boundary

The durable document-watch rows (`document:watch-buffer`, `document:watch-overflow` and the rest of
the fifteen `document:*` rows) stay with the durable packet
([#1180](https://github.com/lanshengzhi/cpp-coding-harness/issues/1180)); this slice builds no
durable async-listener bridge and makes no durable comparison. The remaining `#1178` batches
B1/B2/B4/B6/B7 and [#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044) stay
open.

Magpie-only AI access, XDG state roots, strict no-exception behavior, the ordinary application
Session, all explicit AI/platform exclusions and the no-historical-Pike-durable-migration boundary
remain unchanged. SIGKILL evidence does not establish power-loss or multi-process guarantees.
