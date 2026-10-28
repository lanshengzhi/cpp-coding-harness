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

`Harness::inspect()`:

1. Call-time seal: `session->failed()` -> reject with the frozen message
   (unchanged; pinned by merged #1166 tests).
2. Enqueue a **private line read** -- an internal `Session` primitive with
   the same machinery as `read_committed` (reuse `PendingRead`; no new
   line semantics) but a typed callback and no public surface:
   `session->read_on_line([this](Storage& store) -> Expected<HarnessInspection>`.
   (The zero-API fallback is `read_committed` with a caller-supplied result
   slot; the private seam is preferred for type safety and is what review
   asked for.)
3. The job runs on the line with `mutation_line` held. Inside the job:
   a. `session->failed()` again (frozen `#assertHealthy` parity: reject
      with the seal if the session failed between admission and the run).
   b. Take the Harness mutex; copy `active_invocations`, the scheduling
      flags, and the registry snapshot handle.
   c. `store.scan_tasks(256)` (same cap as today) -- while still inside
      the same job.
   d. Derive every row from that one bundle; return the finished
      `HarnessInspection` as the job's result.
4. The caller consumes the result synchronously. The direct-scan code
   path and the identity-snapshot code path move *into* the job
   unchanged in behavior.

No settled-view cache, no init scan, no source audit: the job reads
storage at the line instant, so there is nothing to keep synchronized
(fact 7). Memory cost: zero. Extra latency: one line round-trip (frozen
pays the same on `readOnLine`).

## 3. Why the ordering requirements hold

- **R1 (no later turn).** The capture happens inside the read job. Jobs
  are mutually exclusive on the line (fact 5) and FIFO by admission
  (facts 5, 6): every commit enqueued before the call either settled
  before the job started (its completion ran on the line, before us) or
  is ahead of us in the queue; every commit enqueued after the call is
  behind us. The scan and the identity copy inside the job therefore
  cannot observe a later turn's effects, and cannot miss an earlier
  turn's effects.
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

## 4. Lifetime

The read job's lifetime is bounded by the synchronous `inspect()` call,
and the call's lifetime is bounded by the caller, who must own the
Harness exclusively (the same contract as every synchronous Harness
method). `~Harness` cannot run concurrently with an in-flight inspect;
and when it does run, `~Impl -> ~Session` destroys the line with no
pending jobs because no inspect job can be pending (fact 1 removes the
v1 drain assumption; caller ownership replaces it). `Harness::close()`
additionally drains or runs queued reads through the audited close path
(fact 2/3), so an inspect racing `close()` either runs during the drain
or rejects with the closed error.

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
  cycle. A future change deferring stop delivery off the line must
  re-examine reentry detection; recorded as a constraint on that change.
- Phases and execution-thread code (not line owners) may call inspect.

## 6. Public discriminating tests

| Item | Test | Today's result |
|---|---|---|
| FIFO enqueue order + capture on line | `fifo-order`: hold a commit turn; a user create is enqueued behind it, then inspect; after release the inspection shows the reservation's effects but NOT the later create; a follow-up inspect shows both | RED today (inspect returns without the queued create) |
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
