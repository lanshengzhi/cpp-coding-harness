# #1080 durable submissions, inbox and busy admission

## Scope

Implement B22 / F4 on the local
`integration/1080-submission-inbox-admission` branch against **pi-v1.3.0 /
71023aec1546d6d677a65669c5665c0927ce0dd0**, Linux x86-64 with glibc.
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

## Validation

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
- Final review fixes and validation are pending; this record does not yet
  claim a successful final Full Validation run.

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
#1044 remains open. Work is local until separately authorized for publication.
