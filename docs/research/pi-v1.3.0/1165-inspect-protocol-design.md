# #1165 inspection protocol design v2 (review draft)

Status: revised candidate for review. No production code yet. Supersedes v1
(the settled-view cache + post-barrier snapshot), which review refuted on
R1: a commit enqueued after the barrier could settle between the barrier
completion and the caller-thread snapshot and land in the view. v2 removes
the cache entirely and captures the inspection inside the line job.

## 1. Audited facts (measured in this worktree / frozen checkout, not assumed)

1. **`Impl::shutdown()` does not close the Session.** It stops the
   scheduler (stop sources, worker join, empty-invocation wait, execution
   joins). The `Session` member is then destroyed by `~Impl`;
   `Session::~Session() = default`. `session->close()` happens only from
   `Harness::close()`. Consequence: the v1 lifetime claim (close drain)
   does not hold for `~Harness`-without-close; v2 rests the lifetime
   argument on caller ownership instead (§4).
2. **Read admission rejects are defined and sealed.** The read producer
   checks `failed` -> `session_failed_error()` and `closed` ->
   `session_closed_error()` at admission, under the line lock
   (`Session.cpp`, read producer). So an inspect after failure gets the
   #1166 seal; after close, a defined closed error; neither hangs.
3. **Failure drains the queue.** `reject_pending_line` visits every
   pending job variant (commit and read) and completes it with the seal
   error; `pump_line` and the settle path call it on failure. A read
   enqueued before a failure and not yet run rejects with the seal.
4. **A read job is pure.** `start_line_job(PendingRead)` runs the
   callback and completes; no storage writes, no seq, no commit-listener
   notifications. The only storage access is what the callback performs.
   There is no "empty transaction" side-effect question if the barrier is
   a read, not a commit.
5. **Line jobs are mutually exclusive and FIFO.** `pump_line` admits one
   job at a time under `mutation_line`; completions re-enter the pump.
   `Session::failed()`/`closed()` take `mutation_line`, which is the
   measured reason inspect today blocks behind a held commit turn.
6. **Frozen `Session.#enqueue` is a promise-tail chain** (job runs after
   the previous job settles); `readOnLine` asserts usable at call time
   and again healthy inside the job; a job enqueued from inside a running
   job lands strictly behind it, so awaiting that later position from
   within the running job can never resolve -- the prohibition is
   structural and silent in frozen (it would hang the chain). The C++
   port makes the same cycle observable (blocking `consume`), so v2
   surfaces it as a Validation error (§5).
7. **Frozen scan-on-line.** Frozen inspection reads its storage on the
   line (`harness.ts` inspect under `readOnLine`); there is no separate
   record cache to keep in sync, and frozen storage is line-owned (no
   external-writer concept at all).

## 2. Protocol (minimal revision)

Measured facts this revision adds: `AsyncResult` is lazy -- a producer
runs at consumption (`start`/`co_await`), not at construction (ADR 0040
header). So the ordering instant of an inspection is its **consumption**,
and a constructed-but-never-consumed result never touches the line. The
read admission path's closed error is the literal "Session is closed",
textually identical to frozen `assertUsable`.

`Harness::inspect()`:

1. Call-time seal: `session->failed()` -> reject with the frozen message
   (unchanged; pinned by merged #1166 tests).
2. Consume a **private line read** -- an internal `Session` primitive with
   the same machinery as `read_committed` (reuse `PendingRead`; no new
   line semantics) but a typed callback and no public surface. Its
   producer assigns an ordering ticket and enqueues in **one** line-lock
   critical section: ticket = `turns_enqueued++`, push behind everything
   with a smaller ticket. (The zero-API fallback is `read_committed`
   with a caller-supplied result slot; the private seam is preferred.)
3. The job runs after every turn with a smaller ticket has fully
   completed (line admission runs jobs one at a time; a turn's job ends
   only when its settle callback returns). Inside the job:
   a. `session->failed()` again (frozen `#assertHealthy` parity: reject
      with the seal if the session failed between admission and the run).
   b. Take the Harness mutex; copy `active_invocations`, the scheduling
      flags, and the registry snapshot handle.
   c. `store.scan_tasks(256)` (same cap as today) -- while still inside
      the same job. The Storage virtual runs under `mutation_line`;
      wrapped backends must not re-enter the Session (same class as
      frozen's documented listener contract, see §5).
   d. Derive every row from that one bundle; return the finished
      `HarnessInspection` as the job's result.
4. The caller consumes the result synchronously.

No settled-view cache, no init scan, no source audit: the job reads
storage at the line instant, so there is nothing to keep synchronized
(fact 7). Memory cost: zero. Extra latency: one line round-trip (frozen
pays the same on `readOnLine`).

## 3. Why the ordering requirements hold

- **R1 (no later turn).** The ticket-and-enqueue critical section makes
  the included set exact: turns enqueued before the read's enqueue (same
  lock acquisition) carry smaller tickets and run strictly before the
  read job; turns enqueued after carry larger tickets and cannot be
  admitted before it. No mutex-fairness assumption is used anywhere: the
  lock serializes ticket assignment and push order directly. The capture
  inside the job therefore cannot observe a later turn's effects and
  cannot miss an earlier turn's effects.
- **R2 (single instant).** Records (one scan), identities, scheduling
  flags, and the registry snapshot are captured inside one job; the only
  interleaving hazard left is invocation cleanup, which runs on execution
  threads and takes the Harness mutex -- so the mutex copy in (3b) is
  serialized against it. Publication/erasure from commit completions is
  ordered by the line. The stale-scan interleaving is structurally
  impossible: there is no read path outside the job.
- **R3 (held turn).** A read started while a commit turn is held queues
  behind it and waits -- the behavior the `[held-release]` pin records;
  now guaranteed by the queue instead of the incidental
  `failed()`-lock blocking.
- **R4 (multi-row).** One scan is one instant for all rows; one commit's
  batch settles before its completion runs, so batch-atomic effects are
  visible atomically.
- **Seal/close.** Facts 2 and 3: call-time seal, in-job seal, failure
  drain, closed rejection. All defined, all hang-free.

## 4. Lifetime (open item, not measured)

What is measured: `AsyncResult` is lazy (a producer runs only at
consumption), the read admission paths reject `failed`/`closed` with
defined errors, and `reject_pending_line` completes pending reads on
failure. What is **not** established: that no caller destroys the Harness
concurrently with an in-flight inspect. `Harness::open`'s documented
contract today only covers Storage lifetime ("the caller keeps `storage`
alive until `close()` completes"); nothing states exclusive ownership of
the handle. v2's lifetime argument is therefore an **assumption to be
settled before implementation**, by one of: (a) documenting exclusive
ownership on `Harness` (the norm for this API shape; needs sign-off), or
(b) proving safety under concurrent destruction. This is deliberately
not claimed as measured.

## 5. Reentry

- Inside the job the callback holds `mutation_line` and takes the Harness
  mutex: line -> Harness mutex, the existing sanctioned order.
- `inspect()` called from inside a line job (transaction body, read
  callback, completion delivery) would enqueue behind its own job and
  block on `consume`: self-wait. Frozen's tail-chain has the same cycle
  structurally (fact 6); v2 detects it with a line-owner thread id set
  around each job (`Session::Impl`, RAII) and returns a Validation error
  naming the reentry. Divergence from frozen: frozen's cycle is a silent
  chain hang; ours is an explicit error. Documented deliberately.
- Stop callbacks currently run on the line (the `[issue1166][reentry]`
  demonstration depends on it), so the owner mark covers the demonstrated
  cycle. What the mark does **not** cover: a callback that delegates to
  another thread which then synchronously waits on the line -- a cycle
  the owner mark cannot see. v2 does not claim that class is cycle-free;
  it is recorded as an open risk, and the reentry-rejection behavior
  itself is a new public contract that needs authorization before
  implementation (see below).
- Frozen precedent for the contract direction: frozen's own listener
  contracts state "It must not block or call Session operations"
  (session.ts, sync listeners and close listeners). Rejecting a
  line-callback reentry with a Validation error extends that documented
  prohibition to a detected error instead of a hang. Whether the port
  adopts the rejection, documents the prohibition without detection, or
  keeps today's incidental behavior is a contract decision for review --
  the tests below pin the chosen behavior once authorized.
- Phases and execution-thread code (not line owners) may call inspect.

## 6. Public discriminating tests

| Item | Test | Today's result |
|---|---|---|
| FIFO enqueue order + capture on line | `fifo-order`: hold a commit turn (admission proven by the gate); consume the inspection during the hold; only then enqueue a second task's create; release. The inspection must wait out the hold, include the held turn's effects, and exclude the later create; a follow-up inspect shows both. Ordering is by admission (consumption instant), never by API-call timing. | RED today (inspect blocks on the seal lock only until the held turn settles, then scans; it neither has a defined included set nor a ticket) |
| R1 regression guard | `capture-on-line`: same shape; if the capture ever moves off the line, the later create leaks into the inspection | RED on any off-line capture |
| Held turn (C) | existing `[held-release]` (kept) | PIN |
| Single instant | existing `[stale-scan]`: with the job-captured scan there is no T1/T2 window; expected GREEN | RED today (3/3) |
| Seal reaches a waiting read | `seal-reaches-waiting-inspect`: gated failing commit; an inspect already queued behind it rejects with the seal (audited `reject_pending_line`) | RED today (returns a non-sealed view or hangs until the gate) |
| Close and waiting reads | `close-drains-waiting-inspect`; post-close `inspect-after-close` returns the defined closed error | RED today (undefined) |
| Reentry | `inspect-in-commit-body` -> Validation error, commit completes; `phase-inspect` from an execution thread -> success | RED today (self-wait) |
| Batch atomicity | `batch-atomic`: one commit creates two tasks; never one-without-the-other | passes; kept |
| External writer | `external-poke`: a direct `MemoryStorage` poke is visible iff it landed before the job's scan instant (frozen has no external-writer concept; this pins our boundary honestly) | new definition |

## 7. Costs, risks, divergences

- The scan runs on the line: a slow storage stalls every read and commit
  behind it. Frozen pays the same cost on `readOnLine`; the
  `[held-release]` pin documents the bounded version of this for us.
- Callers that used inspect as a prompt non-line read now queue: any
  caller invoking inspect from inside a line callback gets a Validation
  error instead of a stale answer (or, today, a block). This is the
  intended contract change and matches frozen's structural prohibition.
- The private read seam is new Session API surface (internal): one
  method over the existing `PendingRead` machinery.
- #1088 coordination: this seam is the observation entry point the view
  work composes on; the `session->failed()` seal stays at the entry per
  the #1166 boundary note.
