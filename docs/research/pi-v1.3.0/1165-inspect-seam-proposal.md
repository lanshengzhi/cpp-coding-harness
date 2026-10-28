# #1165 private line-read seam proposal (for review; nothing implemented)

Companion to `1165-inspect-protocol-design.md`. Answers the review's four
requests: the seam's concrete diff, lifetime ownership, complete
failure/close rules, and the scope of frozen's listener no-reentry
prohibition. No production code is changed by this document.

## 1. Seam

Frozen's equivalent is `Session.readOnLine`, explicitly marked `Internal:`
(session.ts:234) and used by frozen `Harness` for inspection-class reads
(harness.ts:248 `#inspectTask` family). Our port keeps the same package
boundary: the seam is not public API.

### Variant A (recommended first slice): zero new API, only `Harness.cpp`

`Harness::inspect()` routes its existing capture through the existing
`Session::read_committed`, with a caller-side result slot:

```cpp
support::AsyncResult<HarnessInspection> Harness::inspect() {
    if (impl_->session->failed())                      // F1, unchanged
        return support::AsyncResult<HarnessInspection>{std::unexpected(session_failed_error())};
    if (impl_->session->closed())                      // C2/C4: frozen assertUsable text
        return support::AsyncResult<HarnessInspection>{std::unexpected(
            support::make_error(support::ErrorCode::Session, "Session is closed"))};

    struct Slot {
        std::optional<HarnessInspection> value{};
        std::optional<support::Error> capture_error{};
    } slot;

    auto read = impl_->session->read_committed(
            [&slot, this](const std::map<TaskId, TaskRecord>&) -> support::Expected<chord::Json> {
                if (impl_->session->failed())          // F2: frozen #assertHealthy parity
                    return std::unexpected(session_failed_error());
                slot.value = impl_->capture_inspection_on_line(); // mutex copy + scan + derive
                if (!slot.value) {                     // F3: capture failure
                    slot.capture_error = ...;
                    return std::unexpected(*slot.capture_error);
                }
                return chord::Json(nullptr);
            });
    auto outcome = consume(std::move(read));           // caller blocks; job runs on the line
    if (!outcome) {
        if (slot.capture_error) return AsyncResult<HarnessInspection>{std::unexpected(*slot.capture_error)};
        return AsyncResult<HarnessInspection>{std::unexpected(outcome.error())};  // F4 seal
    }
    return support::AsyncResult<HarnessInspection>{std::move(*slot.value)};
}
```

`capture_inspection_on_line()` is today's scan + single-identity-copy +
derive, moved verbatim (the identity copy already takes the Harness mutex
alone and the scan already runs outside it -- verified in current
`Harness::inspect`; no new lock ordering).

Behavior changes this introduces (each needs sign-off, listed in §5):
the inspection is line-queued (the intended fix); a capture failure now
seals the session like any failed read (frozen-aligned; today inspect
does not seal); post-close inspect reports "Session is closed"; an
inspect called from inside a line callback self-waits -- the same hazard
class `read_committed` already has today, not a new one.

### Variant B (follow-up refactor, not the first slice)

A typed `Session` internal read (friend `class Harness`, private method
on `Impl`, type-erased callback because `HarnessInspection` cannot be
named in `Session.hpp` without a cycle), plus a monotonic
`turns_enqueued` counter assigned with the enqueue in one line-lock
critical section for the ticket-ordered included set (fifo-order). Needs
its own authorization; variant A deliberately does not implement
ticketing.

## 2. Lifetime ownership

| Referent | Owner | Why it outlives the read job |
|---|---|---|
| `Session::Impl` | `Harness::Impl` | the job exists only between `consume` start and completion; `consume` bounds the synchronous `inspect()` call; the caller owns the Harness handle (see open item) |
| `Storage&` | caller of `Harness::open` | documented: "The caller keeps `storage` alive until `close()` completes" |
| result `Slot` | caller thread's stack | bounded by `consume` |
| registry snapshot | `shared_ptr`, self-contained | value-captured |
| identity set | value copy | no aliasing |

Open item (restated, not measured): nothing in the current API docs
forbids destroying a Harness concurrently with an in-flight call. The
slice's safety rests on documenting exclusive ownership of the handle
(sign-off required), same as every synchronous method on this class.

## 3. Failure and close rules (complete)

- **F1** `failed()` at `inspect()` entry -> the #1166 seal. (Today; pinned.)
- **F2** session fails after admission but before the job runs -> in-job
  `failed()` check -> seal. (Frozen `#assertHealthy` parity.)
- **F3** the capture's storage access fails -> the read completes with
  that error and the session seals (read-failure propagation, ADR 0040 /
  #1166). Divergence from today (inspect errors do not seal), flagged.
- **F4** a commit fails while a read is queued -> `reject_pending_line`
  completes the pending read with the seal. (Audited path.)
- **C1** `close()` with a queued read -> close's drain runs it while
  storage is still open -> value.
- **C2** `inspect()` after `close()` completed -> "Session is closed".
- **C3** `inspect()` racing `close()`: a read admitted before the close
  turn runs during the drain; the read job holds the line, so `close()`
  blocks on the line lock until the capture finishes.
- **C4** admission finds the session closed -> "Session is closed".
- **D1** `~Harness` without `close()` while an inspect is in flight ->
  excluded by the to-be-documented exclusive-ownership contract; open
  item, not claimed safe.

## 4. Frozen listener no-reentry scope (exact)

- `subscribeCommits`: "It must not block or call Session operations; a
  throw is reported." (session.ts:544)
- `subscribeClose`: "It must not block or call Session operations; a
  throw is reported." (session.ts:563)
- host callbacks run under "must not fail its caller" (session.ts:875).
- `readOnLine` is `Internal:` (session.ts:234); its `#enqueue` tail-chain
  makes an in-job await of a later position structurally unresolvable.

Scope: frozen DOCUMENTS the prohibition for session listeners and host
callbacks; every line callback is covered structurally and silently by
the tail-chain. Frozen does not add a separate text for harness-side
invocation stop delivery; the same structural prohibition applies. Our
port already documents the equivalent obligation for SchedulerHooks
(issue #1166 notes). A DETECTED reentry rejection (Validation error via a
line-owner mark) would be a new behavior beyond frozen's documented
prohibition; it is not part of variant A.

## 5. The authorized minimal slice (stale-scan RED -> GREEN)

Precondition scheme, established from code:

1. Lock order: caller thread -> Session `mutation_line` (via the
   PendingRead job) -> Harness mutex (brief identity copy only) ->
   storage virtual scan outside the Harness mutex. Verified: today's
   inspect already scans outside the mutex, and commit turns already call
   storage under the line, so no lock order or reentry surface is new.
2. Coherence: `run_task_execution`'s cleanup erases an identity only
   after that invocation's final commit COMPLETED (the phase or abort
   handler awaited it); the completion ran on the line; a read job is
   mutually exclusive with line jobs. So at capture time either the
   terminal record settled before our job (scan sees terminal) or the
   erase happens strictly after our copy (identity still live). The
   stale-scan mixed instant is structurally excluded. The early-cleanup
   paths that erase without a terminal (abort reservation, missing task)
   leave today's derive semantics untouched -- parity, not a new hazard.
3. Changeset: `Harness::inspect` only (variant A). No `Session.cpp`
   change, no public header change, no new threads or locks.
4. Sign-offs required before implementing: line-queued inspect; F3
   seal-on-capture-failure (frozen-aligned); C2 message; D1 ownership
   wording.
5. Tests: stale-scan GREEN, closed-admission GREEN, fifo-order-probe
   unhidden and deterministic GREEN, seal-waiting and held pins still
   green, then bounded focused runs. Full Validation only when
   authorized.
