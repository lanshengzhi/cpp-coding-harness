# #1174: Durable JSONL optional fsync and ordered flush failure handling

## Scope

Delivers the frozen `JsonlStorageOptions.fsync` option of the JSONL backend
under [#1180](https://github.com/lanshengzhi/cpp-coding-harness/issues/1180),
[#1183](https://github.com/lanshengzhi/cpp-coding-harness/issues/1183) and
[#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044), under
[ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md).
Selected comparison baseline: `pi-v1.3.0` /
`71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64 with glibc.

The slice implements the option at the existing physical storage boundary of
`cch_durable`. It adds no per-backend policy authority, no scheduler or
Session involvement, and no guarantee beyond the frozen ordering. Whole-row
verification belongs to #1180.

## Frozen contract

Source of truth, read at the pinned revision:

- `packages/durable/src/storage/jsonl/storage.ts` —
  `JsonlStorageOptions.fsync` (default `false`), `commit`,
  `reclaimSidecars`, `replaceSidecar`, `recover`.
- `packages/durable/test/jsonl-storage.test.ts` — the operation-trace
  contract: `orders publication flushes exactly and flushes main only to
  authorize reclamation`, `poisons after {before,after} failure at flush
  {1,2} and never writes a marker`, `defers reclamation after
  authorizing-main flush failure`, `keeps a committed base usable after
  reclaim-temp flush failure`.
- Registered observation `fixtures/pi-v1.3.0/audit-1054/jsonl-store.json` →
  `JSONL fsync flushes sidecar bytes before the main marker is appended`:
  `appendOrder: [doc-3.jsonl, main.jsonl]`, `flushOrder: [doc-3.jsonl]`.

Frozen behavior implemented here:

1. `commit` with fsync appends every sidecar, then flushes every sidecar in
   append order, then appends the main marker. A sidecar flush failure poisons
   the instance and the marker is never appended.
2. Reclamation, on the commit path and during recovery, flushes `main.jsonl`
   first when fsync is on. A failure defers the whole pass silently: the
   commit still succeeds and the store stays usable.
3. Sidecar replacement writes `doc-<id>.jsonl.reclaim`, flushes the scratch
   file when fsync is on, then renames. A scratch-flush failure defers the
   replacement and leaves the scratch file for recovery to delete.
4. Recovery truncates unconfirmed tails first, then applies the same
   main-flush-gated reclamation pass.

**Settled reading of criterion 1, adjudicated by the PM.** "Then flushes the
main marker before publication" is implemented as the frozen reclamation gate:
`main.jsonl` is flushed after the marker append, before a sidecar is replaced
or removed. Flushing `main.jsonl` on every commit after the marker append would
add `main.jsonl` to `flushOrder` and fail the registered discriminator, whose
`flushOrder` is exactly `[doc-3.jsonl]`.

This is not a tolerance in the implementation. Frozen
`storage.ts:reclaimSidecars` returns immediately when the reclamation plan is
empty and only flushes `main.jsonl` when it is not, so frozen pi never flushes
the main marker on the publication path at all. Criterion 1 and criterion 2
cannot both hold literally; the PM adjudicated to keep the frozen behavior and
is amending the criterion's wording to name the reclamation pass.

## Sidecar append-order divergence and its coverage

Frozen pi appends sidecars in write order (`[...encoded.sidecars]` over an
insertion-ordered map); the C++ encoder emits them in sorted filename order
(`EncodedCommit::sidecars` is a `std::map`). The divergence is real for a commit
carrying two or more sidecars.

Its scope relative to this ticket:

- Both registered observations for this baseline carry **one** sidecar:
  `a JSONL commit appends every sidecar before the main marker`
  (`appendOrder: [doc-4.jsonl, main.jsonl]`) and `JSONL fsync flushes sidecar
  bytes before the main marker is appended` (`appendOrder: [doc-3.jsonl,
  main.jsonl]`, `flushOrder: [doc-3.jsonl]`). Neither pins the relative order of
  two sidecars, so criterion 2's discriminator is unaffected and this ticket's
  acceptance does not depend on the divergence.
- The property this ticket does own is structural: flushed sidecars are flushed
  **in the store's own append order**, whatever that order is. That holds for
  any encoder order and is what the publication-order trace pins.
- The publication-order trace creates two sidecars with ascending IDs, so the
  sorted order and the write order coincide there and the trace matches frozen
  pi's expected list. That case therefore does not establish general
  multi-sidecar append-order parity, and it is not claimed to.

Ownership: the relative append order of multiple sidecars belongs to the
append-order rows (`cch_durable:Storage:jsonl:ordering`,
`cch_durable:Storage:jsonl:sidecar`,
`cch_durable:Storage:jsonl:main-marker`), whose registered scenario is also
single-sidecar. Closing that gap needs a new frozen capture of one commit
carrying two document creates plus its `appendOrder`, registered against the
baseline before any row claims it; that capture and any encoder change are
outside this ticket. Changing the encoder here would have altered append order
for every existing JSONL store with no observation authorizing it.

## Delivered behavior

- `src/durable/include/cch/durable/JsonlStorage.hpp`:
  `JsonlStorageOptions{bool fsync{false}}` and
  `open(directory, options = {})`. The class and option docs state the
  ordering only, and explicitly deny a power-loss, SIGKILL or multi-process
  guarantee.
- `src/durable/JsonlStorage.cpp`: the commit path appends every sidecar, flushes
  each in the same order when fsync is on, then appends the marker; a sidecar
  flush failure poisons and returns the poison error before the marker exists.
  `Impl::flush_sidecar` routes task sidecars through `open_task_file(...,
  O_RDWR)` and re-checks the observed identity, so the #1085 no-follow,
  single-link boundary holds for the flush as well as the append.
- One `Impl::reclaim_sidecars` serves both the commit path and recovery: an
  empty plan does nothing, an fsync plan flushes `main.jsonl` first and returns
  silently on failure, and the pass then removes or replaces each sidecar.
  Recovery now builds one plan for document and task sidecars instead of
  applying each file as it walks them, so the fsync gate covers the whole pass
  and a gate failure skips every replacement while `open` still succeeds.
  Truncation stays ungated, as in frozen pi.
- `src/durable/JsonlIoTestHooks.hpp` (new, private): `JsonlIoOperation`
  (`Append`, `Flush`, `Write`, `Rename`, `Remove`), `JsonlIoEvent`,
  `set_jsonl_io_hook`, `clear_jsonl_io_hook` and `fail_jsonl_io`. It follows the
  `src/durable/ObservationTestHooks.hpp` precedent: a private test seam, not a
  public contract. The hook state is file-static behind a mutex and the
  recorder holds no test-case state, so a Catch2 `REQUIRE` that abandons a test
  frame cannot leave a dangling callback (`CODING_STANDARDS.md` §11.10).
- Task-sidecar reclamation emits the frozen `remove:task-<id>.jsonl` operation
  after its identity check and before `unlinkat`; the task append branch emits
  `append:task-<id>.jsonl` before it opens the sidecar.
- `tests/durable/FrozenExpectation.{hpp,cpp}` expose `inputs()` so the test
  reads `inputs.options.fsync` from the artifact instead of restating it, and
  accept a scenario with no `inputs` object.

Recorded divergence, not fixed here: frozen pi appends sidecars in write order,
while the C++ encoder emits them in sorted filename order
(`EncodedCommit::sidecars`). The publication-order trace creates documents in
ascending ID order so the two orders coincide; the coverage scope and the row
that owns the gap are recorded above. Changing the encoder is outside this
ticket.

## Tests

All cases live in `tests/durable/JsonlStorageTest.cpp` and observe the public
`JsonlStorage`/`Session` seam through the private I/O hook:

- `JSONL fsync flushes sidecar bytes before the main marker is appended`
  (`[spec]`): the registered discriminator. The same document commit runs with
  the option disabled and enabled; only the enabled trace flushes, and the
  flush sits between the sidecar append and the marker append. Expected
  operations are built from the frozen `appendOrder`/`flushOrder`.
- `JSONL publication flushes exactly and flushes main only to authorize
  reclamation`: the four frozen publication shapes. Two document creates →
  `append doc-A, append doc-B, flush A, flush B, append main`; a checkpoint
  base → `append doc, flush doc, append main, flush main, write
  doc.reclaim, flush doc.reclaim, rename doc.reclaim->doc`; an entry-only
  commit → `append main` only; a terminal task → `append main, flush main,
  remove task-N.jsonl`.
- `JSONL sidecar flush failure poisons before the marker and never publishes`:
  flush call 1 and call 2 of two. The commit rejects with
  `JSONL storage is poisoned and must be reopened`, `main.jsonl` is
  byte-identical, reads reject, and the reopen shows the pre-change value with
  the unconfirmed sidecar tail truncated and the never-confirmed creation
  sidecar removed.
- `JSONL main flush failure defers reclamation without changing the commit`:
  flush call 2 on a checkpoint. The commit resolves, the in-memory value is
  updated, the sidecar keeps both lines and the store stays usable; the reopen
  with a failing recovery flush emits exactly `["flush:main.jsonl"]`, keeps
  both lines and reads the committed value; a later healthy reopen reclaims to
  one line.
- `JSONL reclaim-temp flush failure keeps the committed base usable`: flush
  call 3 leaves the scratch file behind, the commit resolves, a following delta
  commit still appends, and the reopen reads the delta result with two sidecar
  lines and no `.reclaim` file.
- `JSONL fsync is a per-instance option`: two stores open at once keep their own
  option across interleaved commits; an fsync-written store continues under the
  default; a default-written store continues under fsync.

Acceptance discipline: the trace cases separate the property from the check.
Moving the sidecar-flush loop after the marker append produces test failures,
and flushing `main.jsonl` on every fsync commit instead of only when a
reclamation plan is non-empty fails 5 of the 6; both mutations were applied to
the working tree and reverted before delivery.

## Validation

- Focused: `cmake --build --preset vcpkg --target cch_tests_durable` passed;
  `./build/cch_tests_durable "[issue1174]"` selected **6** cases and passed
  **259/259** assertions. The owning module label `ctest --preset vcpkg -L
  durable` selected **303** cases and passed **303/303**.
- Architecture selection, required because the public Owner Interface gained a
  struct and a defaulted parameter: `ctest --preset vcpkg -L architecture`
  selected **43** cases and passed **43/43** (66.24 s), Parity Architecture
  Gate PASS.
- Full Validation, final: `cmake --build --preset vcpkg` then unfiltered
  `TERM_PROGRAM=herdr ctest --preset vcpkg`: **4065 selected, 4065 passed, 8
  skipped, 0 failed** (214.50 s). `TERM_PROGRAM=herdr` is the prerequisite
  declared by the PTY evidence case itself, which opens with
  `REQUIRE(host_term_program == "herdr")`; the suite is otherwise unfiltered and
  no test is excluded. The 8 skipped cases are the existing optional
  live-pi/SQLite interchange, realtime-clock and Native TUI differential
  skips.
- Two earlier full runs are recorded for honesty. The first reported seven
  failures and the second two; every one of them passed on isolated rerun or
  under the documented host-terminal prerequisite, and none touched
  `cch_durable`. One of those runs reported `staged install contains only the
  relocatable Runtime` as an install-gate staleness failure: a source comment
  edit landed after the last full build, so the Gate correctly refused a stale
  install. The rebuild above is what cleared it. The remaining earlier
  failures (timed-out query partial results, bounded parallel cancellation,
  codemode worker exit, PTY stop drain, projection cost bound, streaming block
  freeze, fullscreen scroll indicator, `/mcp` reconnect) are load-sensitive
  cases in unrelated owners.
- `scripts/format-check.sh bbb0a7f69` reports clean after `git clang-format`
  on the changed C++ files; `git diff --check` passes.

## Capability status

`cch_durable:Storage:jsonl:fsync` advances implementation only. The row's
`verification` status stays `upstream-captured`, and the whole-row comparison
against the registered artifact remains owned by #1180. No consolidation
regeneration was run in this slice, so the ledger counts are unchanged here;
the delivery record is the evidence for the implementation advance.

## Remaining boundary

- No power-loss, SIGKILL or cross-process guarantee, and no cross-process
  locking. The injected flush failures are ordering observations, not crash
  evidence.- A reclamation failure never changes a commit outcome; recovery retries
  deferred reclamation on reopen. Later commits perform their own reclamation
  plans.
- The encoder's sorted sidecar order diverges from frozen pi's write order for
  commits with two or more sidecars. No registered observation for this
  baseline pins that relative order, so it is outside this ticket's acceptance;
  the append-order rows own it and need a new two-sidecar frozen capture first.
  Flush order follows the store's own append order, which is the property this
  ticket delivers.
- Whole-row verification, capability promotion and the ledger rebuild stay with
  #1180. #1044 remains open and is not edited by this slice.
