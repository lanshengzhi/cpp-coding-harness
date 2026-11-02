# #1184 Chord context wait and observer cancellation comparisons

## Scope

Close the four remaining `cch_chord:context:*` cancellation rows of the frozen
`audit-1057` family, capture the missing background/todo root observation, and
record the reviewed mapping for the `cch_chord:Chord:context` composite row.
Authority: [#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044),
[#1178](https://github.com/lanshengzhi/cpp-coding-harness/issues/1178),
[#1183](https://github.com/lanshengzhi/cpp-coding-harness/issues/1183),
[ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md),
Named Baseline **pi-v1.3.0** at `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc.

Bounded scope: the Chord waiter-only context wait and the ordinary durable
Session's watch, acquisition and close cancellation semantics. This slice adds
no document transaction, delta/revision or storage behavior: Storage is
unchanged, the durable on-disk shape is unchanged, and `Session::commit()`
carries its context to observers without performing pi's `#runCommit` abort
check.

The public caller is the frozen one: `chord::Context`, `with_cancel`,
`with_abort_signal`, `without_abort_signal` and the ordinary durable `Session`
(`commit`, `watch_doc`, `DocumentWatch::start`, `DocumentWatch::stop`, `close`).
No parallel Session and no test-only seam was added.

## Delivered behavior

### C++ seams

| Frozen pi surface | C++ seam |
| --- | --- |
| `context/index.ts:awaitWithContext` | `chord::await_with_context(support::AsyncResult<T>, Context)` |
| `context/index.ts` abort error | `chord::context_abort_error(reason)` |
| `Session.commit(callback, context)` context | `Session::commit(change, const chord::Context&)` |
| `watch.start((value, ops, context))` | `DocumentFrameListener(DocumentFrame, chord::Context)` |
| `Session.watchDoc(token, context)` | `Session::watch_doc(definition, address, const chord::Context&)` |
| `Session.close(context)` | `Session::close(const chord::Context&)` |
| `CommittedWatch.observeCancellation` | `DocumentWatch` samples its acquisition context on every delivery and in `stop()` |

`await_with_context()` is the waiter-only race: the caller's cancellation
rejects the wait and the underlying operation still runs to completion, with
only its terminal delivery dropped. A context without a cancellation layer
returns the operation unchanged. Cancelling a watch ends delivery with
`WatchEndReason::Cancelled`, drops buffered frames and leaves committed state
untouched; a watch acquisition cancelled before or during its committed read
registers no observer and writes nothing; a cancelled close caller rejects the
wait while Storage still closes and the Session still ends `Closed`. Every
committed frame carries its commit context masked by
`chord::without_abort_signal()`, so an observer keeps the commit's typed values
and never inherits the committer's cancellation (frozen
`session-watches.test.ts:preserves commit Context values without inheriting
producer cancellation`).

### Complete delivered rows (4)

One delivery record, `1184-chord-context-cancellation`, in
[1058-deliveries.json](1058-deliveries.json), pins the production sources, the
comparison test sources, the frozen artifact and manifest, the evidence tooling
and the CTest JUnit result. Each row's `remainingBoundary` is empty.

| Row | Frozen scenario (`audit-1057/context-cancellation.json`) | C++ case |
| --- | --- | --- |
| `cch_chord:context:await-with-context` | `a context wait rejects with the caller abort reason while the underlying work still completes` | `a context wait rejects with the caller abort reason while the underlying work still completes` (`tests/chord/ChordContextTest.cpp`) |
| `cch_chord:context:watch-cancellation` | `cancelling a watch ends delivery without aborting a completed callback or later commits` | `cancelling a watch ends delivery without aborting a completed callback or later commits` (`tests/durable/SessionWatchTest.cpp`) |
| `cch_chord:context:close-cancellation` | `cancelling the close caller rejects close while storage cleanup still completes` | `cancelling the close caller rejects close while storage cleanup still completes` (`tests/durable/SessionWatchTest.cpp`) |
| `cch_chord:context:acquire-cancellation` | `cancelling a watch acquisition rejects the wait and leaves no write or partial observer` | `cancelling a watch acquisition rejects the wait and leaves no write or partial observer` (`tests/durable/SessionWatchTest.cpp`) |

The durable cases read the registered artifact's own `expected` object through
`FrozenScenario::load()` and compare the frozen fields directly: the frozen
`waitError`/`closeError`/`acquisitionError` `{name, message}`, the frame list
with its `{value, abortSignal, contextValue}`, `closed.reason`,
`valueAfterCancel`, `snapshotAfterCancel`, `storageCloseStarted`,
`storageCloseCompleted` and `sessionEnd.reason`. The Chord case asserts the same
frozen fields as literals, like the other Chord comparisons in
[1178-chord-reconciliation.md](1178-chord-reconciliation.md).

### Discriminating cases

| Row | Discriminating case | Why the comparison alone cannot separate it |
| --- | --- | --- |
| `context:await-with-context` | `an already cancelled context wait still starts and completes the underlying work` (`tests/chord/ChordContextTest.cpp`) | The frozen wait is cancelled while the work is pending, so a waiter that dropped or never started its producer still passes. The case cancels the caller before the wait is installed and requires the work to start anyway and to complete afterwards. |
| `context:watch-cancellation` | `C++ Session cancellation and close with a Context` (`tests/durable/SessionWatchTest.cpp`) | The frozen commit context has no abort signal, so it cannot separate a masked commit context from the raw one. The composite commits under an already-cancelled context and still requires the delivered frame to keep the commit's typed value with no abort signal, then requires a later commit not to reach the cancelled watch. |
| `context:close-cancellation` | `an already cancelled close caller still closes storage` (`tests/durable/SessionWatchTest.cpp`) | The frozen caller is cancelled while cleanup is already running, so an implementation that checks the caller before starting cleanup still passes. The case cancels the caller before `close()` is invoked and requires both the rejection and the started-and-completed storage close. |
| `context:acquire-cancellation` | `a cancelled watch acquisition registers no observer` (`tests/durable/SessionWatchTest.cpp`) | The frozen caller only observes the rejected wait. The case starts the acquisition itself and cancels it inside its committed read, so an implementation that registers the observer before its cancellation check returns a watch instead of the rejection. |

The five discriminating mutations were exercised by hand on the delivered tree
before the record was written, and each one failed exactly the case named for
it: removing the post-read acquisition check, checking the close caller before
cleanup, delivering the raw commit context instead of the masked one, reporting
`Stopped` from a cancelled watch, and not starting the operation of an
already-cancelled wait.

### Normalizations

1. **Abort error mapping.** Frozen pi reports `AbortError` with code 20
   (`ABORT_ERR`) and the generic message `The operation was aborted`; the
   caller's `AbortController.abort(reason)` value lives on the signal, not on
   the error. The C++ mapping is `ErrorCode::Cancelled` (the existing
   cancellation code), the exact frozen message, and the caller's abort reason
   as `Error::detail`. Both the frozen message and the reason are asserted.
2. **Watch acquisition in flight.** The C++ `Storage` seam is synchronous, so
   the frozen "cancel while the committed read is outstanding" window is
   delivered from inside the read (`tests/durable/GatedStorage.hpp`
   `on_find_document`) instead of from a second asynchronous task. The frozen
   observations are identical: the caller gets the frozen `AbortError`, no
   write is issued, and the snapshot is unchanged. The cancellation check runs
   before and after the read, so a cancelled acquisition registers no observer
   at all.
3. **`watch.closed`.** The C++ `DocumentWatch` has no `closed` promise; the
   terminal result is observed through `DocumentWatch::stop()`, which samples
   the acquisition context and keeps the first terminal reason
   (`Cancelled` before `Stopped`). Delivery samples the same context, so a
   cancelled watch delivers nothing even before `stop()` is called.
4. **Frame context.** pi's listener receives `(value, ops, context)`;
   `DocumentFrame` stays the frozen `{value, ops}` shape and the commit context
   is the listener's second parameter.

### `cch_chord:context:root-background` — captured, not promoted

The row has no frozen observation: its audit record declares no comparison
scenario and records "no frozen capture recorded for this row; source-only
audit". The direct capture is the existing case `background and todo contexts
are distinct empty roots` (`tests/chord/ChordContextTest.cpp`), which observes
exactly what the issue asks for: distinct root identity
(`&background_context() != &todo_context()`), the exact pi string forms
`[Context BACKGROUND_CONTEXT]` / `[Context TODO_CONTEXT]`, no abort signal, no
abort reason, no value and no cancellation for either root.

The row stays `upstream-captured` / `unverified`. The consolidation contract
only promotes a row whose exact comparison scenarios map to a registered frozen
artifact, and a C++ observation cannot substitute for a missing upstream
capture. Registering a C++-derived expectation as frozen evidence would be the
normalization the reconciliation forbids.

### `cch_chord:Chord:context` — reviewed mapping, not promoted

The composite row declares one comparison scenario, `C++ Session cancellation
and close with a Context`, with no artifact (`route: declared-comparison`,
`artifact: null`). The issue allows either a matching observation or a reviewed
mapping onto the four rows; this delivery records both:

- The observation is the case `C++ Session cancellation and close with a
  Context` (`tests/durable/SessionWatchTest.cpp`): one real Session driven with
  contexts end to end — a signalling commit context whose value reaches an
  observer without its signal, a cancelled watch whose later commit does not
  reach it, and a cancelled close whose cleanup still completes.
- The mapping is the four verified rows above, whose frozen scenarios are the
  composite's four capabilities.

The row itself keeps its recorded status because its declared scenario has no
registered frozen artifact to compare against.

### Remaining boundary of the carried context

`Session::commit()` carries the caller's context to observers. pi's `#runCommit`
also rejects a commit whose caller is already cancelled before running the
callback; that abort check is not part of any frozen scenario in this slice and
stays unimplemented and unverified rather than being guessed from the C++ side.

## Validation

Measured on this ticket's implementation commit `4e9ad3fbf` and, after the
`origin/main` merge, on the merged tree `2e62fa140` that the receipt pins:

| Command | Result |
| --- | --- |
| `cmake --build --preset vcpkg` | built; build-phase Parity Architecture Gate PASS |
| `ctest --preset vcpkg -N -L issue1184` | 9 selected |
| `ctest --preset vcpkg -L issue1184 --output-on-failure --output-junit docs/research/pi-v1.3.0/deliveries/1184-context-cancellation.xml` | 9/9 passed, 0 failed, 0 skipped |
| `ctest --preset vcpkg -N -L chord` | 79 selected on the merged tree (70 delivered + 3 new Chord cases + 6 from the merged #1185 packet) |
| `ctest --preset vcpkg -L chord --output-on-failure --output-junit docs/research/pi-v1.3.0/deliveries/1178-revalidation-1184.xml` | 79/79 passed, 0 failed, 0 skipped |
| `ctest --preset vcpkg -L architecture` | 43/43 passed |
| `ctest --preset vcpkg -L '^(issue1090\|issue1101\|issue1104)$'` | 36/36 passed |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py build` | ledger rebuilt; 54 delivery-backed rows (4 from this record, 5 from the merged #1185 packet) |
| `python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check` | PASS |
| `python3 tests/evidence/ConsolidationAuditTest.py` | PASS |
| `python3 scripts/evidence/audit_evidence.py verify --baseline pi-v1.3.0` | PASS |
| `TERM_PROGRAM=herdr ctest --preset vcpkg -j1` | three unfiltered runs: 4,226 selected / 4,223 passed / 3 failed, then 4,226 / 4,225 / 1, then (merged tree) 4,232 / 4,231 / 1 (details below) |
| `scripts/format-check.sh b2fa10bb9` | clean |

Two unfiltered serial runs on this host failed different sets, and none of the
failures is a case this slice delivers or modifies:

- First run: `cch_pi_v130_consolidation_check` and
  `cch_pi_v130_consolidation_boundaries` ran before the delivery record above
  was written, so they rejected the not-yet-registered comparison; both pass on
  the receipt tree (see the consolidation rows of this table).
  `Durable local environment reports file process and watch outcomes` (owner
  #1089) failed its 250 ms "a sibling write delivers no event" negative window
  under the serial full-run load and passed 3/3 in isolation on the same tree.
  It is a native file-watch timing case outside this slice: no `Environment`,
  watch or inotify code changed here.
- Second run, on the receipt tree: `Independent nested calls run concurrently
  and deliver in completion order` (owner #1170) failed one completion-order
  assertion under the same load and passed 3/3 in isolation. It is a durable
  concurrency timing case outside this slice.
- Third run, on the merged tree (including the #1185 and #1191 packets):
  `bounded parallel cancellation leaves queued tool capabilities unstarted`
  (owner #497) failed one `duration_ms` timing assertion and passed 3/3 in
  isolation. It is an agent-package concurrency timing case outside this slice.
  Both consolidation gate cases passed in this run.

No assertion, timeout or label exclusion was changed to obtain that result, and
no skipped case supplies acceptance evidence.

## Capability status

Four rows move from `partial` / `upstream-captured` to `implemented` /
`verified`, each through the validated delivery record:

- `cch_chord:context:await-with-context`
- `cch_chord:context:watch-cancellation`
- `cch_chord:context:close-cancellation`
- `cch_chord:context:acquire-cancellation`

`cch_chord:context:root-background` and `cch_chord:Chord:context` keep their
recorded implementation and verification status and now name their bounded
follow-up (a frozen capture for the root row; a registered artifact for the
composite's declared scenario). Membership is unchanged: no row is added,
removed or re-owned, and no neighboring owner's row is promoted. On the merged
tree the ledger moves from 49 to 54 delivery-backed rows: 4 from this record and
5 from the #1185 packet merged alongside it.

## Remaining boundary

The two unpromoted rows and the `#runCommit` abort check are the complete
remaining obligation of this slice. They must not be satisfied by deriving an
expectation from C++, and this packet accepts no adjacent row.
[#1178](https://github.com/lanshengzhi/cpp-coding-harness/issues/1178) batches
B2–B6 and [#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044)
stay open.

Magpie-only AI access, XDG state roots, strict no-exception behavior, the
ordinary application Session, all explicit AI/platform exclusions and the
no-historical-Pike-durable-migration boundary remain unchanged. SIGKILL
evidence does not establish power-loss or multi-process guarantees.
