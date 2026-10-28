# #1165 reservation-visibility slice design (implemented at 98d1d8433)

Second slice on top of the landed line-read seam (`2d9823ba1`). The seam
made inspection a one-instant line capture; this slice fixes the original
defect: the scheduler registers the invocation identity before its
reservation commit settles, so a capture can pair a still-`pending`
record with a live identity and derive `running`.
`[reservation-visibility]` stays RED and visible until this slice lands.

## 1. Publication point (as implemented)

The worker awaits the reservation commit; publication happens after the
await returns, in one Harness-mutex critical section together with the
`closed` check and the execution-thread spawn:

1. the commit settled with an error -> **discard** (the Session also
   seals, #1166);
2. the commit no-oped (the transaction body found the record no longer
   `pending` and not abort-marked, so its `tx.set_task` never ran and the
   `reserved` flag stayed false) -> **discard**;
3. the Harness is closing/closed at publication -> **discard** (frozen: a
   held reservation at close never starts its phase; `[close-held]`);
4. otherwise -> **publish** the identity and spawn the execution thread
   in the same critical section.

There is no additional line-side readback: the settle outcome plus the
`reserved` flag (set only when the transaction staged the write) decide
publication, so no extra Storage read can fail and no decision is left
unset. The execution thread starts only after publication, and its first
instruction re-checks `closed` under the mutex.

## 2. Synchronization primitive (no stack capture)

Implemented variant (approved as "equivalent no-dangling synchronization"):
the reservation commit is awaited on the worker, and publication, the
`closed` check, and the execution-thread spawn happen in **one** Harness
mutex critical section after settlement -- so there is no waiter and no
window between publication and phase start. The execution thread additionally
checks `closed` under the Harness mutex before its first phase instruction
and erases its identity if the Harness closed in between. The shared-state
design below remains the fallback if the spawn point ever moves off the
worker.

The old implementation captured a stack `condition_variable`/flag by
reference in the settle completion, which is a notify-after-unlock and
destruction race (first review's finding). This slice uses a **shared
completion state** owned by both sides:

```cpp
struct ReservationState {
    enum class Decision { Pending, Published, Discarded };
    std::mutex mutex;
    std::condition_variable cv;
    Decision decision{Decision::Pending};
};
auto reservation = std::make_shared<ReservationState>();
```

- The execution thread keeps its own `shared_ptr` copy and waits on it;
  the settle completion captures another `shared_ptr` copy. Neither side
  can dangle the other; the last owner destroys the state.
- Wait predicate: `decision != Pending`. The wait is set by the
  completion in every terminal outcome of the commit (success, failure,
  close discard, no-op discard), so it terminates exactly when the line
  settles the admitted commit -- the same termination guarantee
  `await_async_void(session->commit(...))` already has today. A commit
  admitted to the line is always settled (settle, failure rejection, or
  the close drain runs it); a hung Storage stalls it exactly as it stalls
  every commit today. Test-side waits are all bounded.
- The wait is on the state's own mutex, which is a leaf: no code holds it
  while taking the Harness mutex or the Session line.

## 3. Lock order

- Settle completion (on the line): line -> Harness mutex (publish the
  identity, read the close flag) -> release -> state mutex (set decision,
  notify). This is the existing line -> Harness-mutex direction the
  commit/close listeners already use; the state mutex is a leaf.
- Execution thread: state mutex only (wait); after wake it reads the
  decision and either enters the existing phase loop or returns. It takes
  no lock while holding the state mutex.
- No new edge from the Harness mutex to the line exists (the
  call-path audit in the seam proposal section 3b still holds).

## 4. Close, failure, no-op paths

| Path | Behavior |
|---|---|
| reservation commit fails | discard; Session seals (#1166); thread wakes, ends without a phase; cleanup erases an absent identity (no-op) |
| commit no-ops (record not running) | discard; thread ends; scheduler re-evaluates the task on its next pass |
| close during the commit | the close drain runs the commit; completion sees closing/closed -> discard; phase never starts (`[close-held]`) |
| close after the start check | not claimed windowless: close signals every published identity's stop source before the phase body runs, and the phase's own commits reject once the Session closes, so the phase unwinds through the abort/failure path |
| close before admission | commit rejected at admission; discard |
| shutdown | `shutdown()` marks closed and notifies; the completion of any admitted commit always runs (line settle/drain), so every waiter terminates. `active_invocations` only ever contains published identities, so the shutdown wait cannot be held by an unpublished reservation |

## 5. Resume guard

`waiting -> pending` resume in `schedule_step` must skip a task whose
identity is live (`active_invocations.contains`), because a phase that
committed `waiting` can still be active. With late publication the guard
is exact: the identity exists only while a phase is genuinely running.
Cleanup dirties the scheduler so the resume happens on a later pass after
the identity is gone.

## 6. Inspection consistency

With late publication the reservation-time incoherent pair is impossible:
the identity is published only after the record committed `running`. One
other window still produces (committed `pending`, live identity) as
coherent state: the definition-replacement path writes `running ->
pending` and erases the identity only afterwards. The derive therefore
keeps the explicit order

1. `record.status == "running"` -> running (identity not required;
   abnormal phase end / crash-recovered record);
2. `record.status != "pending" && identity live` -> running (a parked
   `waiting` phase, an abort-marked live task);
3. otherwise derive (`completing`, waiting-on, fit, ready).

A committed `pending` record is never promoted by identity alone;
`[pending-identity]` discriminates it (RED as `running` without rule 2's
pending guard, GREEN as `ready` with it). `terminal` records are skipped
before any derive; the capture keeps the seam's one-instant copy.

## 7. Verification results

- `[reservation-visibility]` was RED at `2d9823ba1` (10/10
  deterministic, log preserved under `1165-evidence/`) and is GREEN
  now. That case is also protected by the seam's line linearization, so
  late publication's own discrimination is carried by
  `[held-release]` (a held reservation publishes nothing until it
  settles), `[rejection]` (a rejected reservation publishes nothing and
  seals) and `[pending-identity]` (a committed pending record is never
  promoted by a live identity).
- Green set after the slice: `[reservation-visibility]`, `[waiting]`,
  `[resume]`, `[identity]`, `[held-release]`, `[seal-waiting]`,
  `[stale-scan-line]`, `[closed-admission]`, `[close-held]`,
  `[running-record]`, `[pending-identity]`, `[rejection]`.
- Bounded focused runs only; gate release precedes every join; no Full
  Validation until a later authorization.
