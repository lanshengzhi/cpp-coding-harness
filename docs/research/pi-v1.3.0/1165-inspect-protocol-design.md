# #1165 inspection protocol design (review draft)

Status: candidate design for review. No production code yet. The frozen
names referenced below (`readOnLine`, `#live`) describe the upstream pi
architecture; nothing here assumes semantic equivalence from a name — each
claimed behavior has a public discriminating test (§8).

## 1. Requirements and measured constraints

R1. **FIFO observation.** An inspection reflects exactly the commit turns
enqueued before the call (frozen `readOnLine` semantics), and no part of a
later turn. Today this fails: the `Session::failed()` seal check orders the
read against the *active* turn only; a commit enqueued before the call but
queued behind the active turn can settle between the seal check and the
scan. New discriminating test: `fifo-order`.

R2. **Single-instant snapshot.** Task records, invocation identities, and
scheduling state from one instant. Today this fails (deterministic RED,
3/3): `stale-scan` pairs a pre-cleanup `waiting` record with a
post-cleanup identity view. The Harness mutex cannot provide this: line
commits settle without ever taking it (measured; the
mutex-across-storage-scan idea is rejected without code).

R3. **Held-turn behavior (C slice, pinned).** An inspect started while a
commit turn is admitted-but-unsettled waits for that turn and observes its
settled effects (`held-release`). The 2s no-return pin proves blocking
within the bounded window only.

R4. **Seal and close.** Post-failure reads reject with the frozen seal
message (pinned by the merged #1166 tests). Close drains or rejects
everything the protocol queues (the original reason queued inspect was
forbidden is answered here: `Session::close` already drains/rejects the
pending line queue — pinned by the #1166 close-drain tests).

R5. **Reentry and lifetime.** Callable from execution threads (stop
callbacks, phases). A call from inside a line turn must not deadlock (a
frozen `readOnLine` from a callback would); it returns a Validation
error. No pending line reads can outlive the Harness.

R6. No new threads or schedulers. Existing lock order only.

## 2. Design: two parts

### Part 1 — settled-view cache

`Harness::Impl` maintains `settled_tasks: map<task id, TaskRecord>`,
updated at exactly one point: the scheduler-side commit-completion chain
that already runs on the Session line after Storage settlement and already
takes the Harness mutex (the #1165 publication completion). For every
successful commit outcome, the completion applies the write batch's
`type == "task"` writes under **one** Harness mutex acquisition:

- `set_task`-shaped write -> upsert; a record whose new status is
  `terminal` -> erase (inspection never shows terminal rows; the settled
  view's size tracks the live set).
- Batched publication: one commit's writes become visible atomically.
- Failure/rollback: rejected callbacks and failed storage outcomes never
  reach the success completion, so the view is untouched. The session
  seal gates all later reads anyway (#1166).
- Source-agnostic: user commits, scheduler reservations, resume commits,
  reconcile writes, phase terminal writes, workflow/compaction task
  writes — every task write reaches Storage through the same settle
  path, so the view needs no per-callsite knowledge. Verified by audit of
  every `StorageWrite{type="task"}` producer (§7) plus the batch-atomic
  test.
- Initialization: one storage scan inside `Harness::open`, before the
  worker starts and before the handle escapes; later completion updates
  are idempotent overwrites (an init-time internal commit that settles
  around the scan writes the same record the scan saw).

### Part 2 — FIFO barrier

`Harness::inspect()`:

1. Call-time seal: `session->failed()` -> reject with the frozen message
   (unchanged, pinned by #1166 tests).
2. Enqueue a **barrier** job on the Session line: an admitted line job
   with an empty transaction and a completion that signals the caller.
   Line admission is FIFO with commits, so the barrier's completion
   implies every turn enqueued before the call has settled. Turns
   enqueued after the call sit behind the barrier and cannot overtake
   (this is the fairness property `failed()`-locking lacks, flagged in
   review).
3. Wait for the barrier completion (bounded by turn durations; a held
   storage stalls the barrier exactly as the pinned C behavior expects).
4. Snapshot: read `settled_tasks`, `active_invocations`, and the
   scheduling flags under **one** Harness mutex acquisition; derive rows
   from that bundle (no storage call in the read path -> no slow-scan
   window; single instant by construction).

Post-close: the line rejects the barrier immediately; inspect returns
that rejection error (defined, pinned). During close, the drain
completes or rejects the barrier, so a waiting inspect returns before
`close()` does (pinned).

## 3. Lock order, reentry, lifetime

- New orders: none. Completions already run line -> Harness mutex; the
  view update joins that existing section. The barrier wait holds no
  locks (a state block owned by the barrier's shared state, signaled from
  the line completion).
- Reentry: Session marks `thread_local in_line_turn` around each line job
  (transaction body, read callback, completion delivery). `inspect` from
  a marked thread returns a Validation error instead of self-deadlocking
  (frozen: would deadlock; divergence documented and safer). Execution
  threads (phases, stop callbacks) are unmarked and supported.
- Lifetime: the barrier is a pending line job; close drains or rejects
  it (existing `finish_close_if_ready` / `reject_pending_line` paths).
  `~Harness` runs the same shutdown. No thread waits on a barrier after
  the Session line is gone because the line signals it during teardown.

## 4. Boundaries

- **External Storage writers**: writes that bypass the Session line are
  invisible to the settled view (and to frozen's `#live`, which is also
  line-fed) until a line commit touches the record. Documented and
  pinned; the test pokes `MemoryStorage` directly.
- **Deleted/archived tasks**: tasks are never un-created; terminal
  records leave the view, matching today's row-skipping.
- **Submissions/conversations**: today's inspection covers tasks +
  scheduling only. Extending rows (the #1088 view work) layers on the
  same completion-update point and snapshot — the coordination note for
  #1088.

## 5. Failure and close behavior matrix

| Interleaving | Behavior |
|---|---|
| inspect call after storage failure | seal error at step 1 (pinned) |
| inspect waiting when a turn fails the Session | barrier rejected with the seal; inspect returns the seal (new pin) |
| inspect during a held commit turn | waits; sees settled effects (pinned C behavior, now by protocol) |
| inspect after `close()` returns | barrier enqueue rejected; defined error (new pin) |
| close while an inspect waits | drain/reject completes the barrier; inspect returns before `close()` (new pin) |

## 6. #1088 coordination

This protocol is the observation seam #1088's view/inspection upgrade
extends: additional row kinds join the settled view at the same single
completion point; view filters compose on the post-barrier snapshot. The
`session->failed()` seal stays at the observation entry per the #1166
boundary note. Capability/ledger promotion is not claimed.

## 7. Audit checklist for implementation

- Every `StorageWrite{.type = "task"}` producer goes through
  `Session::commit`/`commit_now` (no direct `storage->commit` caller
  anywhere in the Harness).
- Every commit outcome path (success, assembly rejection, storage error,
  seal, close rejection) either reaches the success completion exactly
  once or never; the barrier is signaled on all of them.
- Terminal-erase covers user-written terminals, phase terminals, abort
  terminals, reconcile terminals.
- `open()` scan precedes worker start; completion updates are whole-record
  overwrites.

## 8. Public discriminating tests (one per difficulty item)

| Item | Test | Today's result |
|---|---|---|
| FIFO enqueue order | `fifo-order`: held reservation turn; a user create enqueued behind it; inspect during the hold must wait for BOTH, then show both | RED (inspect returns without the queued commit) |
| Held turn (C) | existing `[held-release]` (kept) | PIN |
| Single instant / stale scan | existing `[stale-scan]` (kept) | RED (3/3) -> GREEN by protocol |
| Batch atomicity | `batch-atomic`: one commit creates two tasks behind a gate; never one-without-the-other across repeated inspects | passes today post-release; kept as regression |
| Failure reaches a waiting inspect | `seal-reaches-waiting-inspect`: gated failing commit; waiting inspect returns the seal | RED today (returns stale non-sealed view) |
| Close drains a waiting inspect | `close-drains-waiting-inspect` | RED today (undefined) |
| Post-close inspect | `inspect-after-close` defined error | RED today (scans closed storage) |
| Reentry | `inspect-in-commit-body` -> Validation error; `phase-inspect` from an execution thread -> success | RED today (commit body blocks forever / undefined) |
| External writer boundary | `external-poke-invisible` until a line commit touches the record | new definition, pinned |
| Initialization | reopen with pre-existing tasks listed (extends existing reopen coverage) | covered |
| Terminal lifecycle | existing `[identity]` (kept) | PIN |
| No-op commit | existing session no-op coverage; view untouched | covered |

## 9. Costs and risks

- Memory: one copy of each live task record (bounded; terminal erased).
- The barrier adds one line round-trip to inspect latency (frozen pays
  the same on `readOnLine`).
- The completion grows a view-update section under the Harness mutex;
  scan-cost proportional to the commit's task writes only.
- Risk: an unaudited direct-storage writer bypasses the view (§7 audit +
  boundary test).
- The reentry error diverges from frozen's deadlock-by-construction;
  documented as a deliberate safety improvement.
