# 1057 — Durable-required Chord dependency subset (pi v1.3.0)

Status: **dependency audit complete; later implementation slices recorded separately** (ticket #1057,
parent #1044, ADR 0075). #1061 implements prepared full replacement and mutable application in the
Pike C++ Chord package; its evidence and remaining verification limits are recorded below.

## Result in one paragraph

Frozen pi-durable v1.3.0 imports four public surfaces of `@earendil-works/chord`: the package root,
`/context`, `/delta`, and `/output`. Of those imports, this audit identifies **53 required
capabilities** in seven families: Context/cancellation, document transactions and observation,
delta tracking/application, revisions, replicated state, strict JSON, and bounded output. The
remaining five Chord rows are unrelated plugin/host/RPC/cluster/cell surfaces and are bounded out
under ADR 0075. The dependency audit carries **46 upstream-captured rows**, **7 deliberately
unverified gaps**, and **5 not-applicable exclusions**. No row is marked `verified`: upstream
capture records frozen behavior, while verification requires a completed C++ comparison at the same
scenario boundary. The 1057 bundle contains **33 frozen public-path scenarios** in five artifacts.
#1061 adds separate direct `prepareReplace` and sequential mutable-validation captures against the
same private clean checkout at `71023aec1546d6d677a65669c5665c0927ce0dd0` with exact observed
values. The durable transaction
evidence includes rollback, committed-revision/sequence observation, a stale prepared revision, and
a definition-version mismatch, so a full-snapshot-only substitute cannot satisfy the delta/revision
obligations.

## Authority and scope

| Item | Value |
| --- | --- |
| Frozen baseline | `pi-v1.3.0` |
| Revision | `71023aec1546d6d677a65669c5665c0927ce0dd0` (`Release v1.3.0`) |
| Upstream package | `@earendil-works/chord` version `1.3.0` (`pi-durable` dependency pin `1.3.0`) |
| Host | Linux x86-64/glibc; captured glibc/node/tsx values are in each artifact envelope |
| Private checkout | `/tmp/cch-1057-pi-frozen` (clone of `pi`, detached at the exact baseline, clean) |
| Audit worktree | `/tmp/cch-1044-worktrees/1057`, branch `ticket/1057-audit` |
| Authority | ADR 0075 target package table: `cch_chord` is “durable-required Context, document/delta tracking, state observation and related semantics; not the whole upstream Chord product” |

This is a dependency-subset audit, not a full Chord product audit. `@earendil-works/chord/node`
has **zero** imports from `packages/durable/src` or `packages/durable/test`. The durable package
import counts are 72 root imports, 68 `/context`, 18 `/delta`, and 4 `/output` import statements
(plus two further `/output` comment references). Every required operation row names a frozen
Chord source/test reference and the actual durable caller; the complete machine record is
`1057-chord-durable-dependency-subset.json`.

## Evidence bundle

All artifacts are under `fixtures/pi-v1.3.0/audit-1057/`. The capture script refuses to write unless
the checkout revision is exact and clean, and re-executes under that checkout's `tsx`.

| Artifact | Scenarios | What it establishes |
| --- | ---: | --- |
| `document-transaction.json` | 8 | exact committed publication/watch frames and replay; callback rollback; creation/migration base; version mismatch; structural no-op; document state; immutable snapshots; retirement; fatal storage failure |
| `context-cancellation.json` | 6 | root/child/sibling cancellation; detached cleanup; waiter-only cancellation; watch cancellation; close cleanup; acquisition cancellation |
| `delta-revisions.json` | 9 | tracker revisions; stale preparation; draft revocation; structural/empty no-op; exact op vocabulary; immutable copy-on-write; ordered batches; mutable application; unsafe/malformed validation; overlap |
| `replicated-state.json` | 5 | attached hydration; cursor order; cursor gap; listener failure isolation; pre-start watch buffering/serialization; 100-frame overflow reset |
| `json-output-support.json` | 5 | strict detached copy and omit undefined; rejection classes; UTF-8 byte length and character boundaries; head/tail bounding and incremental buffer; frozen progress rate |

Manifest: `fixtures/pi-v1.3.0/audit-1057/manifest.json`
`manifestSha256 = d56e0cb0f2a8afbb2045de990390d74e7898d50f7dfad9458cd34a7e23a63e6d`

`python3 scripts/evidence/audit_evidence.py check --manifest
fixtures/pi-v1.3.0/audit-1057/manifest.json` prints the exact candidate registry record:

```json
{
  "audit-1057": {
    "manifest": "audit-1057/manifest.json",
    "manifestSha256": "d56e0cb0f2a8afbb2045de990390d74e7898d50f7dfad9458cd34a7e23a63e6d"
  }
}
```

The manifest source hashes cover `capture-1057.mts`, `scenarios.mts`, and `pi.mts`; each artifact
carries its own baseline/revision/ticket/family/capturedAt/environment envelope, exact artifact
bytes, source endpoints, and scenario records. The scenarios declare their comparison policy where
operation spelling is exact for this run but is not documented by Chord as canonical; the recorded
operation effect, frame order, and replay are the durable obligations.

## Durable consumers by required family

### Context, cancellation and waits

`packages/durable/src/session/session.ts` imports `awaitWithContext`, `BACKGROUND_CONTEXT`, and
`withoutAbortSignal`; `harness/tool.ts` imports `awaitWithContext`; `harness/scheduler.ts` imports
`awaitWithContext` and `withAbortSignal`; `harness/harness.ts` imports both
`withAbortSignal`/`withoutAbortSignal`; `harness/view.ts`, `harness/task-graph.ts`, and
`session/observation.ts` import `withoutAbortSignal`; `testing/env-conformance.ts` imports
`BACKGROUND_CONTEXT` and `withAbortSignal`. Durable tests also use `createContextKey`,
`withContextValue`, and `withCancel`.

| Required operation/default | Frozen source/test reference | Durable caller | Captured evidence |
| --- | --- | --- | --- |
| Empty/background roots and root constants | `packages/chord/src/context/index.ts:BACKGROUND_CONTEXT,TODO_CONTEXT`; `test/context.test.ts:provides distinct empty root contexts` | `SessionImpl.commit/close/watchDoc`; Harness | unverified (only a derived root's absent signal is observed) |
| Keyed value layering | `context/index.ts:createContextKey,withContextValue,Context.value`; `test/context.test.ts:layers typed values without modifying parents` | `SessionImpl.#runCommit/#publish`; `session-watches.test.ts:preserves commit Context values without inheriting producer cancellation` | `context-cancellation.json` child scenario |
| Parent-to-child cancellation | `context/index.ts:withAbortSignal,withCancel`; `test/context.test.ts:inherits parent cancellation and isolates child cancellation` | `harness/harness.ts:bind`; `harness/scheduler.ts:waitForTask` | `context-cancellation.json` parent/child scenario |
| Independent child cancellation | `context/index.ts:withCancel`; same test | `session-watches.test.ts` acquisition/cancellation; `harness-tasks.test.ts` caller cancellation | `context-cancellation.json` parent/child scenario |
| Combined external signal | `context/index.ts:withAbortSignal`; same test | `harness/harness.ts:bind`; `scheduler.ts:1291`; `env-conformance.ts` | `context-cancellation.json` parent/child scenario |
| Detached cleanup context | `context/index.ts:withoutAbortSignal`; `test/context.test.ts:masks caller cancellation for mandatory cleanup` | `SessionImpl.close/#runCommit`; `CommittedWatch`; `harness/view.ts`; `harness/task-graph.ts` | `context-cancellation.json` masked scenario |
| Waiter-only cancellation | `context/index.ts:awaitWithContext`; `test/context.test.ts:stops waiting when the invocation is cancelled` | `harness/tool.ts:335`; `scheduler.ts:363,1622`; `session.ts:516` | `context-cancellation.json` wait scenario |
| Watch cancellation | `context/index.ts:withCancel,awaitWithContext`; `replicated-state-delivery.test.ts:unsubscribe drops queued callbacks without joining or aborting the running callback` | `SessionImpl.watchDoc`; `CommittedWatch.observeCancellation` | `context-cancellation.json` watch-cancellation scenario |
| Close caller cancellation | `context/index.ts:withoutAbortSignal,awaitWithContext`; `test/context.test.ts:masks caller cancellation for mandatory cleanup` | `SessionImpl.close`; `harness.ts:465` | `context-cancellation.json` close scenario |
| Acquisition cancellation | `context/index.ts:awaitWithContext`; `test/context.test.ts:stops waiting when the invocation is cancelled` | `SessionImpl.watchDoc`; `CommittedWatch` | `context-cancellation.json` acquisition scenario |

Exact observed cancellation discriminants:

- A child cancelled with `"child-only"` leaves the parent and sibling un-aborted; an inheritor
  created before the parent is cancelled sees `"parent-reason"` and keeps its `"child"` value.
- `withoutAbortSignal` removes the signal but keeps `"cleanup"` readable through the key.
- `awaitWithContext` rejects with `AbortError`/`"The operation was aborted"`; the gated work still
  resolves `"work finished"`.
- Cancelling a watch closes it with `{reason:"cancelled"}`; the already-delivered frame keeps a
  null abort signal and context value `"commit-context"`; watch value remains `{value:2}` while a
  later commit/snapshot reaches `{value:3}`.
- Cancelling `Session.close` rejects the caller with `AbortError`, but storage close starts and
  completes and `session.closed` is `{reason:"closed"}`.
- Cancelling a watch acquisition rejects `AbortError` and leaves the snapshot `{value:1}`; no
  observer or write is created.

These are the “without changing results” obligations for a C++ mapping. Parent/child propagation
can use `std::stop_token`/`std::stop_source` and `src/support/CancellationBridge.hpp`; waiter-only
cancellation can use its Asio cancellation slot and return `ErrorCode::Cancelled` without cancelling
the producer. The existing mechanism does **not** provide keyed context values, a generic
`awaitWithContext`, a cleanup overlay, or observer/close cancellation semantics.

### Durable document transactions and observation

`packages/durable/src/session/session.ts` is the authoritative consumer. `SessionImpl.#runCommit`
does: abort check → callback → `tx.settleSuccess()` → suppress empty writes →
`storage.commit(writes, withoutAbortSignal(context))` → `tx.adopt(seq)` → `#publish` →
`#sweepDocuments`. A storage throw fails the Session. `#publish` emits ordered immutable
`CommitPublication` changes. `packages/durable/src/session/observation.ts` bridges this to
`CommittedWatch` and `CommittedStateSource`; `CommittedWatch` retains at most 100 pending frames and
uses `RETIREMENT_OPERATIONS = [["r", null]]` for retirement.

| Required operation/default | Frozen source/test reference | Durable caller | Captured evidence |
| --- | --- | --- | --- |
| Ordered commit publication and replayable frames | `session.ts:SessionImpl.#runCommit,#publish`; `transaction.ts:Transaction.settleSuccess`; `session-documents.test.ts:adopts by pointer swap and shares operation payloads with the published revision` | `SessionImpl.commit`; `CommittedWatch` | `document-transaction.json` exact-frames scenario |
| Callback rollback | `session.ts:#runCommit`; `transaction.ts:Transaction.#abortChanges`; `session-documents.test.ts:aborts every change when the callback fails; session-checkpoints-migrations.test.ts:rolls migration and edits back with the callback, then coalesces later edits into one base` | `SessionImpl.commit`; `Transaction.#abortChanges` | `document-transaction.json` rollback scenario |
| Creation/migration base | `session.ts:#publish`; `transaction.ts:planDocument,#assemble`; `session-documents.test.ts:creates an initial base on first access…`; `session-checkpoints-migrations.test.ts:Session document migrations` | `SessionImpl.commit`; `CommittedWatch` | `document-transaction.json` version-mismatch scenario |
| Definition-version mismatch | `documents.ts:materializeDocument`; `session.ts:SessionImpl.snapshot`; `session-documents.test.ts:rejects tokens whose semantics or version disagree with the stored incarnation` | `SessionImpl.snapshot/watchDoc` | `document-transaction.json` version-mismatch scenario |
| Structural no-op publication | `session.ts:#runCommit`; `transaction.ts:planDocument`; `session-watches.test.ts:delivers replayable structural no-op commits instead of suppressing them` | `CommittedWatch`; `Tracker.prepare` | `document-transaction.json` structural-no-op scenario |
| Typed document state | `session.ts:SessionImpl.documentState`; `state.ts:replicatedState,AttachedReplicatedState`; `session-watches.test.ts:preserves commit Context values without inheriting producer cancellation`; `replicated-state.test.ts:hydrates at sequence zero when attaching after existing commits` | `SessionImpl.documentState`; `CommittedStateSource` | `document-transaction.json` document-state scenario |
| Immutable historical snapshots | `session.ts:SessionImpl.snapshot`; `transaction.ts:Prepared.value`; `session-documents.test.ts:returns shared immutable snapshots…` | `SessionImpl.snapshot/snapshotAsOf` | `document-transaction.json` snapshots scenario |
| Retirement | `observation.ts:RETIREMENT_OPERATIONS`; `session-watches.test.ts:delivers retirement and does not follow recreation` | `SessionImpl.watchDoc`; `CommittedWatch` | `document-transaction.json` retirement scenario |
| Fatal storage failure | `session.ts:#runCommit`; `transaction.ts:Transaction.#abortChanges`; `session-documents.test.ts:fails the Session after a Storage failure and publishes nothing` | `SessionImpl.commit/snapshot` | `document-transaction.json` storage-failure scenario |
| Pre-start buffering/serialization | `observation.ts:CommittedWatch.start`; `session-watches.test.ts:keeps the acquisition revision until start…; serializes callbacks…` | `SessionImpl.watchDoc`; `CommittedWatch` | `replicated-state.json` watch-buffer scenario |
| Pending-frame overflow | `observation.ts:CommittedWatch`; `state.ts:StateSubscriber.push`; `session-watches.test.ts:collapses 101 pending commits to one root replacement` | `CommittedWatch` | `replicated-state.json` watch-overflow scenario |
| Acquisition cancellation | `session.ts:SessionImpl.watchDoc`; `context/index.ts:awaitWithContext` | `SessionImpl.watchDoc` | `context-cancellation.json` acquisition scenario |

Exact transaction observations that discriminate a real delta/revision implementation from a
full-snapshot-only substitute:

- After an initial base at revision 1, a watcher starts at `{value:1,items:["a"],nested:{count:0}}`.
  Two commits publish sequence 2 and 3. The first frame carries
  `[["s",["value"],2]]`; the second carries
  `[["p",["items"],1,0,["c"]],["s",["nested","count"],1]]`. Applying both frames to the acquisition
  value yields the exact final snapshot `{value:2,items:["a","c"],nested:{count:1}}`. The frames
  have nonempty operations and the publication values match the watcher values.
- A callback that sets `value = 99` and `nested.count = 5` and then throws `"rollback refused"`
  produces **no** publication and **no** watcher frame; the watcher and snapshot remain
  `{value:1,nested:{count:0}}`, and storage still reports one successful commit. A later successful
  commit is sequence 2 with the `value = 2` delta.
- A watcher acquired at definition version 1 receives
  `[["r",{"value":2,"extra":true}]]` when a version-2 commit lands; the underlying publication is
  `{seq:2,version:2,ops:[]}`. Reading the old definition then rejects with
  `Document 2 (1057.versioned) has newer version 2 than 1`.
- A structural no-op (`items.shift(); items.unshift("a")`) still produces two splice operations,
  publication sequence 2, and a nonempty watcher frame even though the final value is equal.
- Retirement produces the canonical `[["r",null]]`, watch end `{reason:"retired"}`, and no snapshot.
- A storage failure (`"disk vanished"`) produces no publication/frame, leaves the prior committed
  revision intact, ends the session with `{reason:"failed"}`, and later snapshots reject with
  `SessionFailed: Session failed after a storage error; close and reopen it`.

A component that only captures full snapshots at commit time cannot produce the stale-preparation
rejection, the exact ordered operation vocabulary, or the distinction between a no-op publication
and an empty change. Those obligations are therefore not reducible to full-snapshot observation.

### Delta tracking, revisions and application

`packages/durable/src/session/transaction.ts` imports `copyJson`, `Draft`, `JsonValue`, `Change`,
`Op`, `Prepared`, `Tracker`, and `track`; it uses `track(value)`, `beginChange()`, `prepare()`,
`adopt()`, and `abort()`. Memory storage uses `applyImmutableBatches`; SQLite uses `apply`;
`harness/view.ts` and `harness/task-graph.ts` use `applyImmutable`; the tool harness uses `overlap`.

| Required operation/default | Frozen source/test reference | Durable caller | Captured evidence |
| --- | --- | --- | --- |
| Tracker revision and adoption | `delta/tracker.ts:Tracker.revision,Tracker.adopt`; `delta.test.ts:keeps a draft alive across await and adopts only a prepared change` | `Transaction.#acquire,Transaction.adopt`; `storage/memory.ts` | `delta-revisions.json` tracker scenario |
| Stale preparation | `tracker.ts:Tracker.adopt`; `delta.test.ts:rejects foreign, stale, and repeated preparations while allowing competing changes` | `Transaction.adopt`; `session.ts:#runCommit` | `delta-revisions.json` tracker scenario |
| Draft abort/revocation | `tracker.ts:Change.abort`; `delta.test.ts:aborts and revokes without changing the committed value` | `Transaction.#abortChanges` | `delta-revisions.json` abort scenario |
| Exact operation vocabulary (`s/a/p/m/d/t/r`) | `delta/index.ts:Op,assertValidOp`; `delta.test.ts:emits append and rolling-window operations; supports root replacement, root splice, and permutations` | `Transaction.adopt`; SQLite materialization | `delta-revisions.json` vocabulary scenario |
| Empty vs structural no-op | `tracker.ts:Tracker.prepare`; `delta.test.ts:normalizes a deep no-op to exact previous identity` | `transaction.ts:planDocument` | `delta-revisions.json` no-op scenario |
| Immutable copy-on-write | `delta/index.ts:applyImmutable`; `delta-apply-immutable.test.ts:copies each touched container once while preserving input and payload ownership` | `harness/task-graph.ts:200`; `harness/view.ts:237`; `storage/memory.ts` | `delta-revisions.json` immutable scenario |
| Ordered batches | `delta/index.ts:applyImmutableBatches`; `delta-apply-immutable.test.ts:shares one private copy-on-write scope across batch partitions` | `storage/memory.ts`; SQLite materialization | `delta-revisions.json` batch scenario |
| Mutable materialization | `delta/index.ts:apply` (root handling via `isBase`/`isReplace`); `delta.test.ts:applies mutable and immutable operations` | `storage/sqlite/storage.ts:654` | `delta-revisions.json` mutable scenario |
| Unsafe paths | `delta/index.ts:assertSafePath,UnsafePathError`; `delta.test.ts:rejects unsafe and malformed paths` | Memory/SQLite storage | `delta-revisions.json` validation scenario |
| Operation/splice validation | `delta/index.ts:assertValidOp`; `delta.test.ts:rejects unknown verbs, malformed tuples, paths, and splice payloads; rejects gaps, huge indices, and string-spelled indices` | Memory/SQLite storage | `delta-revisions.json` validation scenario |
| Bounded overlap | `delta/index.ts:overlap`; `delta.test.ts:finds bounded overlaps` | `harness/tool.ts:457` | `delta-revisions.json` overlap scenario |
| Root replacement/base detection | `delta/index.ts:apply` (root replacement path); `delta.test.ts:supports root replacement, root splice, and permutations` | `observation.ts:RETIREMENT_OPERATIONS`; SQLite | `delta-revisions.json` mutable/root scenario |
| Prepared replacement | `tracker.ts:Tracker.prepareReplace`; `delta.test.ts:emits an owned root replacement without traversing large replacement input` | `transaction.ts:planDocument` | unverified direct capture |

Exact delta discriminants:

- Two preparations from base revision 0; adopting the first moves to revision 1 and value
  `{value:2,items:["a"]}`. Adopting the second rejects `Prepared change is stale`; value and
  revision remain unchanged.
- After `change.abort()`, writing through an escaped draft handle throws
  `TypeError: Cannot use a settled overlay`; the tracker value is still `{value:1}` at revision 0.
- An untouched change prepares with `ops: []` and can still adopt (revision 1); a structural
  rewrite prepares with two splice ops even though its value equals the base.
- The observed vocabulary is:
  `["s",["o","a"],2]`, `["a",["s"],"d"]`,
  `[["p",["a"],1,1,[]],["p",["a"],1,0,["X","Y"]]]`,
  `["m",["a"],[2,1,0]]`, `["d",["o","b"]]`, `["t",["s"],3]`,
  `["s",["o"],{"b":2}]`.
- `applyImmutable` leaves the base byte-identical, copies touched containers, and preserves
  untouched subtree identity (`next.retained === base.retained`). Ordered batches apply to a final
  revision only; mutable `apply` mutates in place except for root replacement, which returns a new
  object.
- `__proto__` throws `UnsafePathError: unsafe path segment: __proto__`; unknown verb `x` throws
  `TypeError`; a string splice index is rejected before mutation.

### Replicated state and observers

`packages/durable/src/session/session.ts`, `harness/view.ts`, and `harness/task-graph.ts` call
`replicatedState(source, { onError })`; `documentState` and views return
`AttachedReplicatedState`; `observation.ts` supplies `CommittedStateSource` and `CommittedWatch`.
Durable never calls the mutable `replicatedState(initial)` overload.

| Required operation/default | Frozen source/test reference | Durable caller | Captured evidence |
| --- | --- | --- | --- |
| Attached hydration and exact frames | `state.ts:replicatedState,AttachedReplicatedState,ReplicatedStateSource`; `replicated-state.test.ts:hydrates at sequence zero…; publishes exact source values…` | `SessionImpl.documentState`; `CommittedStateSource` | `replicated-state.json` hydration scenario |
| Cursor-gap detection | `state.ts:AttachedReplicatedState.#receive`; `replicated-state.test.ts:reports cursor gaps, disposes the broken attachment, and ignores later frames` | `CommittedStateSource`; `documentState` | `replicated-state.json` gap scenario |
| Listener failure isolation | `state.ts:StateSubscriber`; `replicated-state-delivery.test.ts:isolates a synchronous hydration failure…`; `replicated-state.test.ts:reports listener failures without throwing them into the source` | `documentState`; `task-graph.ts` `onError` | `replicated-state.json` listener scenario |
| Serialized delivery | `state.ts:StateSubscriber.drain`; `replicated-state-delivery.test.ts:serializes reentrant hydration and update callbacks` | `CommittedWatch`; views | `replicated-state.json` watch-buffer scenario |
| Pending bound/overflow | `state.ts:StateSubscriber.push`; `replicated-state-delivery.test.ts:excludes a running update from overflow…` | `CommittedWatch`; `documentState` | `replicated-state.json` watch-overflow scenario (durable watcher bound) |
| Idempotent disposal | `state.ts:AttachedReplicatedState.dispose`; `replicated-state.test.ts:keeps attachments independent and disposes each idempotently` | `documentState`; views | `replicated-state.json` hydration/gap scenarios |

Observed state discriminants: hydration is `{kind:"hydrate",sequence:0}` then
`{kind:"update",sequence:1}`; a skipped cursor reports
`Replicated state source cursor has a gap: expected 1, received 2`, disposes once, and leaves
`{value:1}`; a throwing listener is reported twice (`"listener failed"`) while another subscriber
receives hydrate 0 and update 1. A watch acquired before start buffers frames and invokes callbacks
serially (`values [1,2]`, max concurrency 1); 101 pending commits collapse to one root-replacement
frame with value 101.

### Strict JSON and bounded output

`copyJson` is consumed by `documents.ts`, `harness/usage.ts`, `harness/inbox.ts`, and other
harness modules; `JsonValue`, `Draft`, and `JsonRepresentation` are pervasive document/tool types.
`utf8ByteLength`, `characterEnd`, `boundOutput`, `OutputBuffer`, `Progress`, and
`PROGRESS_BYTES_PER_SECOND` are consumed by `harness/tool.ts`, `truncate.ts`, and `tools/read.ts`.

- `copyJson` deep-copies with no alias. Default strict copy rejects `undefined`; with
  `{omitUndefinedProperties:true}` it drops object `undefined` but still rejects an undefined array
  entry. It rejects functions, non-finite numbers, class instances, and sparse arrays with distinct
  `TypeError` messages.
- `utf8ByteLength("abc"/"é"/"😀"/"aé😀")` is `3/2/4/7`; `characterEnd` over the seven bytes of
  `"aé😀"` including the terminal index is `[0,1,1,3,3,3,3,7]`.
- `boundOutput("abcdefghij", maxBytes 4, maxLines 1, head/tail)` returns
  `{text:"abcd",bytes:4,droppedBytes:6,droppedLines:0}` and
  `{text:"ghij",bytes:4,droppedBytes:6,droppedLines:0}`. `OutputBuffer` pushes `"abc","def","gh"`
  all return true and snapshot as `{text:"efgh",droppedBytes:4,droppedLines:0}`.
- `PROGRESS_BYTES_PER_SECOND = 102400`.

## Current C++ mechanics: what maps, what does not

No `cch_chord`, `cch_durable`, document/revision/delta runtime, or replicated-state attachment
exists in the current tree. The relevant existing mechanics are:

| Required obligation | Existing C++ mechanic | Limit |
| --- | --- | --- |
| Parent/child cancellation propagation | `src/support/CancellationBridge.hpp:CancellationSignalBridge` bridges `std::stop_token` to an Asio `cancellation_signal`; tests in `tests/agent/ToolCallExecutorTest.cpp` cover sequential/parallel cancellation | No keyed Context, no cancellation reason layering, no independent child scope API |
| Waiter-only cancellation | `interruptible_sleep(duration, stop_token, message)` returns `ErrorCode::Cancelled` and leaves the timer/work producer alone; `tests/agent/ToolCallExecutorTest.cpp` covers cancellation boundaries | Only a sleep helper; no generic awaitable wait or `withoutAbortSignal` cleanup overlay |
| Detached cleanup / close | `withoutAbortSignal` has no C++ equivalent; current `AgentSession` owns lifecycle directly | A cancelled close currently has no generic detached cleanup path; must not abort storage work |
| Durable transaction/document observation | `src/coding_agent/include/cch/coding_agent/ProjectionStream.hpp` (ADR 0052) provides Base + ordered value-bearing patches and mailbox resync; `tests/coding_agent/ProjectionStreamTest.cpp` covers attach/convergence/overflow | Projection is an in-process read-model stream, not a document revision source: no document identity, definition version, `Op` batch, revision adoption, stale preparation, or storage durability |
| Delta tracking/application | None. `support::JsonValue` is a value carrier only; no `Draft`, `Tracker`, `Prepared`, `apply`/`applyImmutable`/`applyImmutableBatches` | Projection `apply_projection_patches` is not a substitute: it replaces read-model slices, does not apply durable JSON operations or preserve revision identity |
| Replicated state | None. `TaskStore`/`TaskScheduler` are task records/scheduling, not replicated document state | No source attachment, snapshot cursor, gap handling, `onError`, or hydration/update delivery |
| Strict JSON | `support::JsonValue` + `read_json`/`write_json`; `tests/support/JsonValueIoTest.cpp` pins Glaze-compatible parsing/serialization | Parser is not a strict JSON-value validator (trailing content tolerated), writer maps non-finite to `null`, object keys are sorted and `undefined` is unrepresentable |
| Bounded output | `src/support/include/cch/support/BoundedText.hpp` (`bounded_utf8`, `utf8_tail_start`, `bounded_utf8_tail`) and `src/support/include/cch/support/OutputLimiter.hpp`; `tests/support/BoundedTextTest.cpp`/`tests/support/OutputLimiterTest.cpp` cover UTF-8 boundaries and head/tail truncation | No Chord `bytes/droppedBytes/droppedLines` result, no incremental `OutputBuffer`, no `Progress` pacing; these are analogous mechanics, not interchangeable contracts |
| Progress rate | None | Only the frozen constant is captured; pacing behavior remains unverified |

No existing mechanic is accepted as an empty owner or substitute engine. In particular,
`ProjectionStream` cannot stand in for Chord document observation: it has no durable revision or
delta application path and its convergence tests do not exercise rollback, stale prepared changes,
or definition-version mismatch.

## Proposed `cch_chord` implementation slices

The owner name and scope come from ADR 0075; this audit does not alter the manifest or create the
package. Implementation should be split so each slice has independent callers and focused tests:

1. **Context/cancellation substrate.** Immutable keyed context values, independent child scopes,
   parent propagation with stable reason, a cleanup overlay (`withoutAbortSignal`), and a
   waiter-only `awaitWithContext` equivalent. Map propagation to `std::stop_token`/`stop_source`
   and `CancellationSignalBridge`; return `ErrorCode::Cancelled` for the waiter without cancelling
   the producer. Focused test: parent/child/sibling, masked cleanup, wait-vs-work completion,
   watch/close/acquisition discriminants from `context-cancellation.json`.
2. **Delta/revision engine.** Validated `Op` tuples (`r/s/a/d/t/p/m`), `Draft`, `Change`,
   `Prepared`, `Tracker`, immutable revision adoption, stale-preparation rejection, draft
   revocation, `apply`, `applyImmutable`, `applyImmutableBatches`, and `overlap`. Focused test:
   exact vocabulary/effect, structural no-op vs empty, stale/abort, copy-on-write sharing, unsafe
   path and malformed tuple prevalidation.
3. **Replicated state and observers.** `ReplicatedStateSource`/attachment/frame, snapshot cursor
   hydration, attached state, per-subscriber serialized bounded delivery, gap/`onError`, disposal,
   and the durable `CommittedStateSource`/`CommittedWatch` bridge with the 100-frame reset and
   retirement batch. Focused test: hydration/update, cursor gap, listener isolation, pre-start
   buffering, overflow reset, cancellation.
4. **Durable document transaction integration in `cch_durable`.** The Session commit sequence,
   draft settlement, storage admission, pointer-swap adoption, ordered publications, watcher frames
   with exact operations, document state, immutable snapshots, version mismatch, rollback and
   fatal-failure behavior. The C++ document transaction must call the real chord substrate; it may
   not hide behind full snapshots.
5. **JSON/output support.** Strict JSON validation/copy with a declared `undefined` policy,
   UTF-8 byte length and character boundaries, head/tail bounding with exact dropped counts,
   incremental output buffer, and the progress constant. Add `Progress` pacing only after a
   timing-stable measured test; do not normalize timing away.
6. **Measured representation/cost probe (later, unfiled).** Measure `JsonValue` representation,
   operation-batch size, revision retention and checkpoint base economics before choosing a
   storage delta representation or checkpoint threshold. This ticket deliberately does not decide
   those costs or invent a speculative engine.

Proposed dependency edges (subject to the manifest/architecture checks when implementation lands):
`cch_durable -> cch_chord`; `cch_chord` may use `cch_support` only for Pi-neutral mechanics
(UTF-8/bounded text/value carriers), not for durable document or transaction ownership. No
`cch_durable -> cch_coding_agent` edge and no `cch_chord` dependency on the application runtime.

## Bounded exclusions

Only unrelated Chord product surfaces are excluded; no required durable semantics are excluded.
The five excluded rows are:

- `cch_chord:excluded:plugin-host` — `host.ts`/`loader.ts` plugin/facet host.
- `cch_chord:excluded:rpc-links` — `link.ts`, `connection.ts`, `remote.ts`, `websocket.ts`
  links/RPC transport.
- `cch_chord:excluded:cluster` — `cluster.ts` reload/child reconciliation.
- `cch_chord:excluded:cells-schema` — `cell.ts`/`schema.ts` cell and schema tooling.
- `cch_chord:excluded:node-host` — `node.ts`/`loader.ts` Node host adapter.

`@earendil-works/chord/node` has no durable import. Cloudflare-specific host/deployment adapters
remain excluded; generic durable storage, transactions, recovery, execution and the required
document/delta/state semantics are not excluded.

## Implementation update from #1061

The #1061 frozen bundle is registered in `fixtures/pi-v1.3.0/audit-1061/manifest.json` and adds
direct `prepareReplace` and operation-by-operation validation observations. C++ tests compare
prepared-base retention, ownership, no-op normalization, stale adoption, abort, mutable partial
writes after a later invalid operation, and caller-visible root-replacement behavior.
`delta:prepare-replace` and `delta:mutable-application` are recorded as implemented and
`upstream-captured`; the ledger records their direct C++ tests while keeping that verification
status until a same-boundary frozen comparison is accepted. Checkpoint predicate policy remains
outside #1061 and is not implemented by the Chord slice.

## Unverified gaps (not exclusions)

The seven unverified required rows are:

1. `context:root-background` — direct root identity/string/default capture; only a derived root is
   observed.
2. `document:checkpoint-bases` — checkpoint predicate/base selection and retained delta tail.
3. `document:retention` — document cache eviction/retention timing.
4. `state:pending-bound` — state-subscriber overflow with an in-flight callback (the durable
   watcher overflow is captured).
5. `json:json-value` — a direct C++-comparable `JsonValue` model scenario.
6. `json:json-representation` — compile-time representation mapping has no runtime scenario.
7. `output:progress` — throttled `Progress` pacing/timing.

These are recorded as `unverified`, never as Deferred or excluded. The audit also has no row marked
`verified`: `upstream-captured` records a bundle-pinned frozen observation, and a row becomes
`verified` only when a committed C++ comparison accepts the same scenario; the seven gaps remain
visible.

## Preserved constraints

- `cch_ai` Magpie discovery/catalog/authentication/inference contract is untouched.
- XDG configuration/state roots and historical evidence remain untouched.
- Existing third-party TS/JS runtime exclusion and the no-old-Pike-migration decision are
  untouched.
- #1057 did not change shared registry or production alignment files; #1061's later changes are
  identified in the implementation update above.
- #1057 introduced no production alignment code, manifest owner, or C++ target.

## Validation and handoff

Commands run (from `/tmp/cch-1044-worktrees/1057` unless noted):

```bash
PI_CHECKOUT=/tmp/cch-1057-pi-frozen PI_BASELINE=pi-v1.3.0 \
  /tmp/cch-1057-pi-frozen/node_modules/.bin/tsx \
  fixtures/pi-v1.3.0/audit-1057/capture-1057.mts

python3 scripts/evidence/audit_evidence.py check \
  --manifest fixtures/pi-v1.3.0/audit-1057/manifest.json

jq -e . docs/research/pi-v1.3.0/1057-chord-durable-dependency-subset.json
```

Capture printed five artifacts and the manifest. `check` printed the candidate registry record
above and exited 0. The ledger is valid JSON and its counts are 58 rows: 53 supported / 5 excluded;
33 missing / 18 partial / 2 implemented; 46 upstream-captured / 7 unverified / 5 not-applicable.

The serial merger should register only this record in
`fixtures/pi-v1.3.0/baselines.json` under `baselines["pi-v1.3.0"].bundles`, after verifying the
manifest hash:

```json
"audit-1057": {
  "manifest": "audit-1057/manifest.json",
  "manifestSha256": "d56e0cb0f2a8afbb2045de990390d74e7898d50f7dfad9458cd34a7e23a63e6d"
}
```

#1057 itself made no shared registry or production alignment changes; later issue #1061 owns the
Chord implementation and evidence updates described above. Integration merge and issue close-out
remain separate delivery steps.
