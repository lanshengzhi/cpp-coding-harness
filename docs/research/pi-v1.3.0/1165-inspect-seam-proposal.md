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
                            if (impl->session->failed())
                                return std::unexpected(session_failed_error());
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

`capture_inspection(Storage&)` is today's inspect body moved verbatim
(failed-gate, scan, single identity copy under the Harness mutex, derive)
minus its `AsyncResult` wrapper; the mutex is released before the scan
exactly as today, so the lock order (line -> Harness mutex -> storage
outside the mutex) and the reentry surface are unchanged from today's
commit turns, which also call Storage under the line.

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
- **S3 (new hazard, needs a decision):** an inspect called from inside a
  line callback (commit body, read callback, completion) would queue
  behind its own job and self-wait. Today it merely scans. Options:
  (a) document the prohibition in `inspect`'s contract (frozen
  listener-style); (b) have the seam detect the line-owner thread (one
  thread-id set around job execution, seam-local) and reject with a
  Validation error. Recommendation: (b), contained entirely in the seam,
  with sign-off; shipping a new silent deadlock is worse.
- **S4 (documentation):** `inspect`'s contract must state that the
  Harness must outlive the operation until its completion returns (the
  same duty `read_committed` already documents for the Session). This is
  documentation, not proof; D1 (concurrent destruction) remains an open
  item and is not claimed safe.

Not in this slice: ticketing/`fifo-order` included-set ordering
(`[fifo-order-probe]` stays out of the suite), close-drain (racy pass
remains non-evidence), and any change to `read_committed` or its
contract.

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

## 5. Slice verification plan

`[stale-scan]` GREEN; `[closed-admission]` GREEN (S2 signed);
`[seal-waiting]` and held pins still green; `[fifo-order-probe]` run
manually as evidence only; bounded focused runs per slice. Full
Validation only on authorization.
