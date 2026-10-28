# #1165 private line-read seam proposal v2 (variant B; review draft, nothing implemented)

Variant A (routing inspect through public `Session::read_committed`) is
REJECTED and recorded as rejected: that method's public contract says the
callback "reads committed Storage state only", so sampling harness runtime
state there is a contract violation, and bundling F3/C2 behavior changes
into the stale-scan fix is not acceptable. This revision is design only.

## 1. Seam shape: the package's existing detail-access pattern

The durable package already exposes internal access through a friended
`detail::*Access` struct: `Session.hpp` forward-declares
`detail::DocumentDefinitionAccess`, friends it, and `Session.cpp` owns its
definition (`Session.cpp:381`). Variant B uses the same pattern with a
detail header so `Harness.cpp` can call it:

**New header** `src/durable/include/cch/durable/detail/SessionLineAccess.hpp`

```cpp
#pragma once

#include <cch/durable/Session.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <functional>

namespace cch::durable::detail {

/// Internal, non-public line access for the Harness (issue #1165). A job
/// admitted here joins the same mutation-line FIFO as `read_committed` and
/// commits: it runs after every job admitted before it has settled, and no
/// job admitted after it starts before its completion callback returns.
/// Unlike the public `read_committed`, the job may sample caller-owned
/// runtime state as well as committed Storage state; that is the point of
/// the seam, and it is why it is not public API. Admission rejects a
/// failed Session with the frozen failure message and a closed Session
/// with the frozen closed message; a job error rejects the operation
/// without failing the Session (same rule as committed reads).
struct SessionLineAccess {
    static support::AsyncResult<void> read_on_line(
            Session& session, std::move_only_function<support::Expected<void>(Storage&)> job);
};

} // namespace cch::durable::detail
```

**`Session.hpp`** (private surface only; no public API change)

```cpp
namespace detail {
struct DocumentDefinitionAccess;
struct SessionLineAccess;          // + this forward declaration
}
...
class Session {
    ...
private:
    friend struct detail::SessionLineAccess;   // + this friend
    ...
};
```

**`Session.cpp`** (implementation; reuses `PendingRead` verbatim)

```cpp
support::AsyncResult<void> detail::SessionLineAccess::read_on_line(
        Session& session, std::move_only_function<support::Expected<void>(Storage&)> job) {
    auto& impl = *session.impl_;
    std::lock_guard lock(impl.mutation_line);
    if (impl.failed) {
        return support::AsyncResult<void>{std::unexpected(session_failed_error())};
    }
    if (impl.closed) {
        return support::AsyncResult<void>{std::unexpected(session_closed_error())};
    }
    // Same producer shape as Session::read_committed (Session.cpp:2316):
    // the job is wrapped into the existing PendingRead variant, so it uses
    // the same queue, pump, failure rejection, close drain, and
    // inline-when-idle completion path. No new line kind, no new queue.
    return support::AsyncResult<void>{support::AsyncProducer<void, support::Error>{
            [session = &session, job = std::move(job)](
                    support::AsyncCompletion<void, support::Error> completion) mutable noexcept {
                auto& impl = *session->impl_;
                std::lock_guard lock(impl.mutation_line);
                if (impl.failed) {
                    completion(std::unexpected(session_failed_error()));
                    return;
                }
                if (impl.closed) {
                    completion(std::unexpected(session_closed_error()));
                    return;
                }
                impl.enqueue_line(Session::Impl::PendingRead{
                        .read = [job = std::move(job)](Storage& storage) -> support::Expected<chord::Json> {
                            auto outcome = job(storage);
                            if (!outcome)
                                return std::unexpected(outcome.error());
                            return chord::Json(nullptr);
                        },
                        .completion = [completion = std::move(completion)](
                                              support::Expected<chord::Json> outcome) mutable noexcept {
                            if (!outcome)
                                completion(std::unexpected(outcome.error()));
                            else
                                completion(support::ExpectedVoid{});
                        }});
            }}};
}
```

**`Harness.cpp`** (producer shape, no blocking, no latch)

```cpp
support::AsyncResult<HarnessInspection> Harness::inspect() {
    return support::AsyncResult<HarnessInspection>{support::AsyncProducer<HarnessInspection, support::Error>{
            [impl = impl_.get()](support::AsyncCompletion<HarnessInspection, support::Error> completion) noexcept {
                struct Capture {
                    std::optional<HarnessInspection> value{};
                };
                auto capture = std::make_shared<Capture>();
                auto read = detail::SessionLineAccess::read_on_line(
                        *impl->session, [impl, capture](Storage& storage) -> support::Expected<void> {
                            // No Session method may be called here: failed()/
                            // closed() take mutation_line (Session.cpp:2030/
                            // 2037), and this job already runs on that line.
                            // Failure and close are covered by admission and
                            // reject_pending_line (section 2).
                            capture->value = impl->capture_inspection(storage); // today's body, one instant
                            return {};
                        });
                std::move(read).start([capture, completion = std::move(completion)](
                                              support::ExpectedVoid outcome) mutable noexcept {
                    if (!outcome)
                        completion(std::unexpected(outcome.error()));
                    else
                        completion(std::move(*capture->value));
                });
            }}};
}
```

### Capture body, line by line (today's `Harness::inspect` minus the failed gate)

| Today's line | In the job | Instant / lock |
|---|---|---|
| `if (impl_->session->failed())` | **DELETED from the job** | Session `failed()` takes `mutation_line` (Session.cpp:2030); the job already holds it. Admission checks `failed`/`closed` at call and at producer run; a failure while queued is delivered by `reject_pending_line`. The eager call-time check also disappears: `inspect()` is now a producer, and admission runs at consumption -- the same observable outcome. |
| `impl_->storage->scan_tasks(256)` | `storage.scan_tasks(256)` via the job parameter | line held, Harness mutex NOT held (same as today) |
| `impl_->registry.snapshot()` | unchanged | plain member read, no lock |
| `{ std::lock_guard lock(impl_->mutex); scheduling = closed/enabled; copy active_invocations }` | unchanged | line -> Harness mutex (the only new edge; matches the existing commit/close listener edge) |
| per-row `impl_->inspect_task(task, snapshot, invoked)` | unchanged | pure function, no locks |
| build `HarnessInspection` | unchanged | -- |

So the lock order is: session line -> Harness mutex (brief) -> Storage
scan outside the Harness mutex. That matches today's commit turns (which
call Storage under the line) and today's commit listener (which takes the
Harness mutex from the line).

## 2. Same PendingRead/FIFO, failure, close, drain semantics (proof by reuse)

| Property | Basis |
|---|---|
| FIFO position | the job IS a `PendingRead` in the existing `pending_line` deque; `enqueue_line`/`pump_line` are untouched |
| Admission checks | identical failed/closed checks as `Session::read_committed` (both at call time and at producer run) |
| Failure while queued | `reject_pending_line` completes the pending read with the seal (audited; pinned by `[seal-waiting]`) |
| Job error | rejects the operation WITHOUT failing the Session -- the documented committed-read rule (`start_line_job(PendingRead)`, Session.cpp:571) |
| Close with queued read | the close drain runs pending line jobs while Storage is still open (`finish_close_if_ready` path); unchanged |
| Inline when idle | same inline completion as `read_committed` |
| No new machinery | no new queue, no new thread, no new lock, no new line kind |

## 3. Public behavior changes this slice needs signed off

- **S1 (the fix):** `Harness::inspect` becomes a pending, line-ordered
  operation: it waits for every job admitted before it and observes one
  committed state. Today it scans immediately on the caller thread.
- **S2:** post-close inspect reports the frozen "Session is closed"
  instead of the storage backend's closed error (the seam's admission
  check). Frozen parity, but a public behavior change; the alternative
  is a deliberate closed-passthrough that preserves today's backend
  error -- not recommended.
- **S3 (documentation only in this slice):** `inspect`'s contract states
  that it must not be called from inside a Session line callback (commit
  body, read callback, completion): such a call queues behind its own job
  and self-waits. This is the same prohibition class as frozen's
  listener contracts. No thread-mark detection is implemented: it would
  cover only same-thread reentry, create false safety, and is new public
  behavior. A callback that delegates to another thread which then calls
  inspect remains an UNRESOLVED cross-thread cycle risk.
- **S4 (documentation):** `inspect`'s contract must state that the
  Harness must outlive the operation until its completion returns (the
  same duty `read_committed` already documents for the Session). This is
  documentation, not proof; D1 (concurrent destruction) remains an open
  item and is not claimed safe.

Not in this slice: ticketing/`fifo-order` included-set ordering
(`[fifo-order-probe]` stays out of the suite), close-drain (racy pass
remains non-evidence), and any change to `read_committed` or its
contract.

## 3b. No Harness-mutex -> line edge exists (call-path audit)

The new inspect job adds the edge line -> Harness mutex. Inversion would
require some path that holds the Harness mutex while waiting on the
session line. Every Harness-mutex hold site was audited:

| Site | Holds mutex while | Calls Session/Storage? |
|---|---|---|
| `run_worker` wait (Harness.cpp:1397) | cv.wait on `stop/closed/dirty` | no; mutex released while waiting |
| `schedule_step`/`reconcile` guards (1481, 1517, 1343 listener, 1116 idle) | short state checks | no; `session->commit` is called strictly OUTSIDE these scopes |
| `notify_waiters` (1094), `settle_idle` (1116) | state erase/move | no; completions run after the scope |
| commit listener (1320-1355) | identity lookup | runs ON the line -> line->mutex, the same direction as inspect |
| close listener (1280) | flags/notify | no Session call |
| `shutdown` wait (1382) | cv.wait `active_invocations.empty()` + joins | mutex released while waiting; joins only after the predicate holds, so no thread still needs the mutex |
| `settle_orphaned`/`settle_faulted` (1051/1071) | nothing: `session->commit` runs outside any mutex scope | -- |

Conclusion: no path holds the Harness mutex while enqueueing or waiting
on a line job, so line -> Harness mutex cannot invert against an
existing Harness-mutex -> line wait.

### `AsyncResult::start` semantics and blocking

- Ready result (admission rejected `failed`/`closed`): `start` delivers
  the completion inline on the caller thread; no line interaction.
- Pending result, idle line: the producer enqueues and `pump_line` runs
  the job inline; the completion chain runs nested on the caller thread
  under the line lock (recursive; no other lock is held).
- Pending result, busy line: the producer blocks on `mutation_line`
  until the active job settles -- exactly the existing
  `Session::read_committed` producer behavior (Session.cpp:2316), and
  commits block the same way. The producer contains no loop or
  unbounded wait of its own; a hung Storage stalls it exactly as it
  stalls every commit today.

## 4. Rebuild plan (old production is not approved)

Implementation starts from the merge base `15fd43556`, not from the
unapproved production state:

- `5057091d3` (reservation publication) and `15724cd09` (waiting-resume
  guard) are explicitly DROPPED, not retained by default. Their pins
  (`waiting-active`, the reservation-identity tests, the waiting-resume
  lifecycle tests) must be re-scoped: the merge-base behavior returns
  until a replacement is specified.
- The seam fixes capture coherence (`[stale-scan]`); it does NOT fix
  reservation visibility, because the invocation identity is still
  registered before its reservation settles at `15fd43556`. That
  behavior needs its own re-specification and sign-off -- it is the
  original #1165 defect and is out of the first slice.
- The WIP tests and evidence on this branch are the reviewable record;
  the implementation branch carries the slice plus the re-scoped pins.

## 5. Slice verification plan (implemented; see the implementation commit)

Implemented on the approved rebuild base: the seam, S1, S2, S4 docs.
Measured results:

- `[stale-scan-line]` GREEN 5/5 deterministic: while the inspection's scan
  is held inside its line job, a terminal commit started on another thread
  is line-blocked (record still pending, commit unsettled), and after the
  release the inspection's row still reflects the record it observed while
  the raw record settles terminal.
- `[closed-admission]` GREEN (S2).
- `[held-release]`, `[seal-waiting]`, `[close-drain]`, `[waiting]`,
  `[resume]`, `[identity]` all green.
- `[stale-scan]` is the HISTORICAL direct-scan RED and is hidden
  (`[.][stale-scan-legacy]`): under the seam the held scan owns the line,
  so its own terminal commit can never settle before the release and the
  test would deadlock by construction.
- `[reservation-visibility]` remains RED: the seam makes the capture one
  instant but does not change identity registration timing. The dropped
  production's rejection pin is hidden (`[.][rejection-legacy]`) because
  it was written against 5057091d3's reservation-commit shape.

Coherence claim corrected by the test design: the seam guarantees
record-instant coherence for line-owned writes and blocks later commits
for the scan's whole duration. It does NOT serialize identity erasure
against a scan already in flight (cleanup runs on execution threads under
the Harness mutex), so the record/identity pairing skew remains part of
the out-of-slice reservation-visibility work.
