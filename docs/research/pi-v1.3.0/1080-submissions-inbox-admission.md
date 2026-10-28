# #1080 durable submissions, inbox and busy admission

## Scope

B22 / F4 was integrated into `main` by merge `bc35299b9` against
**pi-v1.3.0 / 71023aec1546d6d677a65669c5665c0927ce0dd0**, Linux x86-64
with glibc. The subsequent delivery audit is recorded below.
The public caller is the independent `cch::durable::Harness`; the ordinary
application Session and `pike --resume` retain their existing storage path.

## Delivered behavior

- Submission records carry identity, conversation, request key, type, placement
  and settlement. Memory, JSONL and SQLite Storage support lookup, request-key
  lookup, scanning and transaction-staged placement/settlement.
- `Harness::submit` returns the existing submission for a repeated request key,
  including when the new payload differs. A key reused for the wrong submission
  type fails. Duplicate and busy-rejected requests write no additional batch.
- Idle input starts a generation run. Passive display-only writes settle without
  starting a run. Busy inputs queue as steer or follow-up, or return `Busy` for
  reject. A queued submission and its inbox document delta commit in one batch.
- The postTools boundary admits steering inputs to the active run while keeping
  follow-ups queued. The final boundary places follow-ups for the next run.
- Submission waiters observe committed settlement, reject on Session close or
  persistence failure, and support cancellation of one wait independently of
  the underlying submission. Conversation abort settles queued inputs as
  unanswered/aborted while retaining queued writes.

Implementation: [Harness.cpp](../../../src/durable/Harness.cpp),
[WorkflowRuntime.cpp](../../../src/durable/WorkflowRuntime.cpp),
[Session.cpp](../../../src/durable/Session.cpp), and the Storage backends.
Public values and operations are in
[Workflow.hpp](../../../src/durable/include/cch/durable/Workflow.hpp) and
[Harness.hpp](../../../src/durable/include/cch/durable/Harness.hpp).

### Frozen comparisons and distinguishing cases

[HarnessSubmissionsTest.cpp](../../../tests/durable/HarnessSubmissionsTest.cpp)
compares the registered
[audit-1055 submission/inbox observations](../../../fixtures/pi-v1.3.0/audit-1055/submissions-inbox.json)
and [audit-1080 boundary observations](../../../fixtures/pi-v1.3.0/audit-1080/submissions-inbox.json).
The latter adds five observations captured independently from the frozen
upstream runtime, including same-key/different-payload deduplication, postTools
versus final placement, and abort effects on queued input versus write.
[reset-writes.json](../../../fixtures/pi-v1.3.0/audit-1080/reset-writes.json)
adds three observations for stale versus current head pins, a queued reset at
postTools, and a display-only write queued behind a reset. All eight scenarios
were recaptured together against the clean frozen revision; the original five
scenario names, inputs, expected values and normalizations compare identically
to the earlier capture. Only their capture timestamp changed.

The held model/tool gates separate admission from execution: assertions inspect
queue and record states before release, then observe the relevant boundary.
The atomic-admission comparison inspects the actual storage batch, rather than
accepting two successful final reads as evidence of atomicity. Storage tests in
[SubmissionStorageTest.cpp](../../../tests/durable/SubmissionStorageTest.cpp)
cover staged resolution and backend persistence separately from Harness policy.

### Delivery audit after integration

The issue had no delivery comment when this audit began. The existing public
`Harness` boundary test compares the postTools/final transcript and answer
identities; the successful queued-admission test records both the submission
and inbox change in one storage batch. Those cases are retained.

- The same-key/different-payload test previously checked identity, zero new
  batches and inbox size, but did not count work. It now also counts one
  submission, one user transcript entry and one assistant answer after the
  original settles.
- The conversation-abort test loaded its registered frozen scenario but did
  not compare its values. It now compares the before and after statuses,
  reasons and retained inbox mode against that observation; the queued input
  is unanswered/aborted while the queued write remains queued. Loading a
  fixture alone was not a frozen comparison.
- The new failed busy-admission case observes the backend immediately before
  the failed Session closes it: neither a submission for the request nor an
  inbox change is visible. The fault double records the rejected batch; the
  test checks the request ID and the actual inbox document's `items` change,
  establishing that the injected failure hit the intended admission. It also
  checks the failed Session rejects a later submission. Together with the
  successful same-batch case this covers
  submission-granularity atomicity and a discriminating failure through the
  public caller. The fault double rejects the entire batch before delegating;
  it does not simulate a physical write failing midway. Backend rollback is
  covered separately by #1074 and #1075.

No new frozen observation or capability-status claim is needed for these
comparisons. The existing audit-1080 observations remain the authority.

### Review corrections

The final review-fix merge is `1ed33cf66` (implementation commit `dff0672e1`).

- Submission abort reads the record, chooses its outcome and withdraws the
  inbox item in one Session transaction. The regression constructs an abort
  while the input is queued, lets the real postTools boundary place it, then
  initiates the abort transaction and observes `already_placed`.
- Conversation abort withdraws queued inputs and reads the live run in one
  transaction, waits for that task, and checks again for a rollover. Task-wait
  failures propagate to the caller. The follow-up-run case checks that abort
  leaves the conversation idle.
- Missing or malformed persisted inbox fields return typed validation errors;
  rejected operations leave records unchanged and the Session remains usable.
- Head lookup grows beyond its initial 4,096-entry window. A regression buries
  the head behind 4,100 newer headless entries and checks stale/current pins.
  The O(n) scan and upgrade trigger are documented in
  [runtime-capacities.md](../../runtime-capacities.md).
- Reset detection records the reset placed at a boundary before removing it
  from the inbox. PostTools, final and admission reset paths clear the old
  replay cache before starting the next run.
- Wait cancellation uses a move-only callback with a weak waiter reference,
  removing the callback/Waiter ownership cycle. Exactly-once claims use the
  default sequentially consistent atomic ordering.
- Harness callers, task worker scheduling, and storage completions can enter
  from different threads. `Session` protects commit enqueueing, settlement,
  and publication across threads on its `mutation_line` recursive mutex without
  holding locks across asynchronous waits. `StorageState` locks all queries,
  scans, and updates with an internal recursive mutex and provides atomic
  `apply_next` batch application for `MemoryStorage::commit`, eliminating
  concurrent iteration data races against background task scheduling.
  `concurrent Harness admissions serialize every submission on the Session mutation line`
  verifies multithreaded admission and settlement.

The review's initial assertion that the stale check preceded numeric parsing
was incorrect: parsing already preceded it. The final code nevertheless uses
an already decoded `WriteEntry` directly, avoiding redundant JSON conversion.

The recovery worker timed out with uncommitted changes. Its claim that all
11 selected regressions passed was not accepted: the log contained only ten
completions. The remaining test misused a same-domain storage gate across
threads. Replacing that gate with the existing held-model/tool handshakes
made the ordering deterministic and completed the test. No timeout or assertion
was relaxed to obtain a pass.

## Validation

Delivery audit validation (2026-10-28, strict no-exception Debug):

- Final clean revision `8304bf612f28adf780a87b7785df0f8fe61b1431`:
  full build passed; unfiltered CTest selected **3,853**, with **3,852 passed,
  0 failed, 1 skipped** in 185.86 seconds. All **43** architecture tests
  passed. The skipped Native TUI differential capture requires its separately
  selected environment. Logs and JUnit are retained locally at
  `/tmp/cch-1080-final-build.log`, `/tmp/cch-1080-final.log`, and
  `/tmp/cch-1080-final.xml`.

- Focused `ctest --preset vcpkg -L issue1080 --timeout 20`: **36 selected,
  36 passed**, including one new case and two strengthened cases.
- Full preset build and configure/build architecture gates passed. The
  unfiltered full run selected **3,853**: **3,851 passed, 1 failed, 1
  skipped** in 747.44 seconds. Architecture tests, including the warning
  gate, passed. This is not a passing full validation.
- The failure was `Native TUI fullscreen scroll-to-end indicator paints on a
  direct-fullscreen launch while the transcript is scrolled` (#1039): after
  the indicator click, an expected scrollbar edge was absent. Ten isolated
  repeats passed; the cause remains undetermined. The Native TUI differential
  capture was skipped. Original full log and JUnit are retained locally at
  `/tmp/cch-1080-full.log` and `/tmp/cch-1080-full.xml`; isolated rerun log is
  `/tmp/cch-1080-tui-repeat.log`.
- The rejected-batch identification was added after that full run. The
  `issue1080` selection passed **36/36** before and after the following
  temporary production mutations; the source was restored, rebuilt, and the
  worktree was clean. These are discriminating probes, not new frozen
  observations:
  - Bypassing the request-key lookup branch produced two submissions and two
    final user entries. The submission count and final user count both failed,
    along with identity, batch count, status and inbox assertions. The final
    assistant count did not fail; no claim is made for that predicate.
  - Removing the queued-write exclusion from conversation abort caused the
    write to settle `unanswered` with a reason and disappear from the inbox.
    The frozen queued status, absent reason and inbox size checks failed.
  - Both targeted cases completed without timeout. Their local logs are
    `/tmp/cch-1080-mut-dedup.log` and `/tmp/cch-1080-mut-abort.log`;
    the restored focused log is `/tmp/cch-1080-restored-focused.log`.
- An initial parallel fresh-tree build encountered PARITY-6002/6003 while
  Ninja updated dependency evidence; serial retry passed. This separate build
  gate race is tracked by #1162. The final full validation above follows the
  batch-identification change.

Initial integration revision: `b2714debc`.

- Worker focused runs: #1080 **25 test cases / 528 assertions**, #1079
  **5 test cases / 212 assertions**; complete durable shard **84 cases /
  2,043 assertions**. Architecture checks: **43/43**.
- Parent incremental `cmake --build --preset vcpkg`: passed.
- Parent `scripts/format-check.sh $(git merge-base main HEAD)`: clean.
- Initial unfiltered full run selected **3,767** tests: **3,764 passed,
  2 failed, 1 skipped**, 171.01 seconds. Both failures were consolidation
  checks: the five new audit-1080 scenarios lacked explicit registration in
  the consolidation plan, so deterministic rebuilding differed and coverage
  was incomplete. This run is not acceptance of the final revision.
- Named-evidence verification after registration: **28 bundles, 83 artifacts,
  488 scenarios** at the exact frozen revision.
- Consolidation deterministic `build` / `check`: passed, **1,464 rows**,
  **485 non-bootstrap scenarios**, **83 explicit orphans**, **0 uncovered**;
  all **39** consolidation boundary tests passed.
- Existing #1101 receipt refresh: inspected `ctest -N -L issue1101`, then
  **14/14 passed** in 1.01 seconds. The result is pinned in
  [1080-registration-1101.xml](deliveries/1080-registration-1101.xml).
- Final review-fix selection: `ctest --preset vcpkg -N -L 'issue1080|issue1079'`
  selected **40**; the matching run passed **40/40** (35 #1080 and 5 #1079),
  with a 15-second per-test liveness backstop. The formerly hanging abort
  regression passed separately in **0.01 seconds**.
- Final integration configure/build and branch-format check: passed.
- Crash investigation on queue-mode test #253: Helgrind identified concurrent
  access between background task scanning and Session/StorageState mutations.
  Synchronizing the Session mutation line and StorageState brought Helgrind
  reports from 1,296 errors to 0 data races. 100 consecutive runs of both
  `queue modes decide whether all follow-ups or one are placed at the final boundary`
  and the new concurrent admissions test passed with zero failures.
- Final unfiltered Full Validation selected **3,777** tests: **3,776 passed,
  0 failed, 1 skipped**, 158.80 seconds. All architecture tests (**43/43**)
  passed. Format check `scripts/format-check.sh` is clean.

The expected Native TUI differential skip requires its separately selected
capture environment and supplies no new terminal acceptance evidence.

## Capability status

The F4 frontier evidence now points to this implementation and its comparison
cases. The consolidation plan registers new observations explicitly without
changing source audit facts, membership, or whole-row verification. This bounded
delivery does not provide complete pinned receipts for every composite durable
capability row; their implementation/verification statuses remain unchanged.

Adding these observations changes the consolidation plan pinned by the existing
#1101 MCP receipt. Review confirmed that the plan delta only adds eight explicit
#1080 observations, and that all other pinned input digests still match.
Its scenario mapping and implementation inputs are unchanged. The exact 14-case
comparison was rerun; only the plan digest and validation record were refreshed
before rebuilding the ledger. The previous JUnit artifact is retained. No row's
membership, implementation status or verification status changed.

## Remaining boundary

Workflow reopen/recovery (#1082), dynamic registry hooks (#1087), and full
views/task graph/inspect (#1088) remain separate slices. This delivery makes no
power-loss, fsync-failure, multiprocess, historical Pike import, or real terminal
emulator claim. Magpie-only AI access and the XDG state root remain unchanged.
#1044 remains open. The implementation is integrated; this delivery audit
remains on its local ticket branch pending integration and acceptance.
