# Session committed-read ordering (#1164)

## Scope

The public `Session::read_committed` seam matches the signature in the paused #1088 worktree and orders its synchronous read callback with Session commits. This does not resolve #1088's separate reservation publication inconsistency.

## Frozen observation

An independent probe at `/tmp/cch-1164-probes/frozen-read-order.ts` ran against the read-only frozen pi-v1.3.0 tree at `71023aec1546d6d677a65669c5665c0927ce0dd0`. A gated `MemoryStorage` returned an unsettled promise from `commit`; the probe released or rejected it explicitly. The probe exited 0 and recorded its output at `/tmp/cch-1164-probes/frozen-read-order.out`.

A read admitted after a held commit did not run or settle until the commit completed, and then saw its conversation. A later commit ran after the read. Close drained admitted reads before closing storage and rejected later reads. A failing commit delivered its own storage error, and queued reads and commits rejected with `SessionFailed`; the read callback did not run. Frozen `session.ts` implements both `commitWith` and `readOnLine` through `#enqueue`, with failure recovery on the tail.

## Delivered behavior

Commits and committed reads share a FIFO queue. A read admitted through `start()` waits for earlier storage settlement, adoption, publication and completion; later jobs wait for its callback and completion. Close seals admission and drains the queue; a storage failure rejects queued jobs in order. A read callback's own error is returned to that read. As with existing Session commit completions, callbacks execute on the recursive mutation line, and callers must keep Session and Storage alive through completion. The seam has no cancellation token: abandoning an unstarted lazy result admits no operation; once started, it has no independent cancellation mechanism.

## Validation

The lock-only implementation matching the paused #1088 seam was tested before the queue change. The selection contained nine tests: four passed and five failed, with fourteen failing assertions. The original reproducer observed an early read and count zero; additional failures discriminated later-commit ordering, failure rejection, close drainage and the mixed queue. This mutation does not establish that every possible queue defect is caught. Raw RED output: `/tmp/cch-1164-probes/test-naive.log`.

After the queue change the same selection passed 9/9. The durable shard passed 159 cases and 4,973 assertions. The issue1101 selection, run after the formatting change, passed 14/14. All 51 delivery receipt input hashes matched and no changed file intersected those inputs; receipts were not rebound. Full incremental build exited 0 with the Parity Architecture build gate passing. Unfiltered CTest selected 3,861, passed 3,860, failed 0 and skipped 1 (Native TUI differential capture); raw output is `/tmp/cch-1164-probes/ctest-full.log`. After that run, test formatting and comments were corrected; the durable shard rebuilt successfully and the issue1164 selection passed 8/8. Format check used merge base `6a51db9fd45206d1a6879683bcc634037fde8668`.

## Remaining boundary

The snapshot family was not changed. A reproducible C++ visibility test in `tests/durable/SessionTransactionTest.cpp`, `a prepared document draft stays invisible until storage settles its commit`, holds a document commit and synchronously calls `snapshot` before release; it obtains the previously cached committed value. Frozen `snapshot` also serves a matching cache hit without enqueueing. For a cache miss, however, frozen `session.ts` enqueues the load on the same tail, while C++ `Session::snapshot` calls `committed_value` directly under `mutation_line` even when storage commit is in flight. A follow-up should gate a commit that creates a previously uncached document and compare a concurrent cache-miss snapshot before and after release; this record does not claim that comparison has been run. Other snapshot, document and watch acquisition paths require their own frozen-contract analysis.
