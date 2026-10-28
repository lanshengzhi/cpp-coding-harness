# #1165 reservation-visibility slice design (small; for review, nothing implemented)

Second slice on top of the landed line-read seam (`2d9823ba1`). The seam
made inspection a one-instant line capture; this slice fixes the original
defect: the scheduler registers the invocation identity before its
reservation commit settles, so a capture can pair a still-`pending`
record with a live identity and derive `running`.
`[reservation-visibility]` stays RED and visible until this slice lands.

## 1. Publication point

The reservation commit's completion is the only publication site:

- It runs on the Session line, inside the commit settlement
  (`start_line_job(PendingCommit)` completion), after Storage settled and
  the record is committed.
- Publication decision, evaluated on the line in this order:
  1. outcome is an error -> **discard** (the Session also seals, #1166).
  2. the Harness is closing/closed -> **discard** (frozen: a held
     reservation at close never starts its phase; `[close-held]` pins it).
  3. the committed record read back is not `running` for this task ->
     **discard** (no-op: task already terminal/aborted/not pending).
  4. otherwise -> **publish**: insert the identity into
     `active_invocations` and mark the state `Published`.
- The execution thread never starts the phase before `Published`.

## 2. Synchronization primitive (no stack capture)

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

With late publication the incoherent pair is impossible: the identity is
published only after the record committed `running`, so a capture seeing
`pending` cannot see a live identity. The capture keeps the seam's
one-instant copy; `terminal` records are skipped before any derive.

## 7. Verification plan

- `[reservation-visibility]` stays RED and visible until the slice
  lands (RED evidence preserved under `1165-evidence/`).
- GREEN set after the slice: `[reservation-visibility]`,
  `[waiting]`, `[resume]`, `[identity]`, `[held-release]`,
  `[seal-waiting]`, `[stale-scan-line]`, `[closed-admission]`,
  `[close-held]`.
- Re-evaluate the hidden `[rejection-legacy]` pin against the restored
  commit shape; unhide if it matches, otherwise keep it documented.
- Bounded focused runs only; gate release precedes every join; no Full
  Validation until a later authorization.
