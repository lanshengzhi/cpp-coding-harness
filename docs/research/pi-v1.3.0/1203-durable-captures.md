# #1203 durable rows captured at pi-v1.3.0

## Scope

Capture the 21 supported `cch_durable` rows that had no v1.3.0 frozen
observation at all (`evidence.route: unobserved` in the 1058 consolidated
ledger) and register the resulting family. The rows cannot be compared,
receipted or honestly statused until an independent frozen capture exists;
this record owns the capture and the first comparison slice.

## Delivered behavior

- The `audit-1203` family executes the frozen checkout's own durable runtime
  through the public Harness seam over in-memory storage with the frozen faux
  provider: harness settings defaults and object-merge rules (`settings.json`),
  background threshold compaction and overflow retry (`compaction.json`), the
  background ownership boundary and idle waits (`ownership.json`), task hooks,
  outcomes, scheduler outcomes and versioning (`tasks.json`), tool task
  cancellation (`tool-task.json`), tool search persistence (`tool-search.json`),
  steering placement, follow-up drain modes and duplicate-request admission
  (`steering.json`), the native extension seam (`extensions.json`), the node
  execution environment watch (`env-watch.json`), the built-in bash tool limits
  (`tool-limits.json`), and the durable codemode limits, models, output and
  restart replay (`codemode.json`). 21 scenarios, 11 artifacts.
- Every listed row's obligation is linked in the consolidation plan, so all 21
  rows now route to a registered frozen observation (`related-captured`).
- Three rows gained C++ comparisons in the owning suites as `[issue1203]`
  cases that read the registered artifacts: the background-ownership boundary
  in `HarnessOwnershipTest.cpp`, the duplicate-request admission in
  `HarnessSubmissionsTest.cpp`, and the harness settings defaults in
  `HarnessWorkflowTest.cpp`. These three rows are delivery-backed verified by
  this receipt.

## Validation

- Capture: `PI_CHECKOUT=<frozen> PI_BASELINE=pi-v1.3.0 <frozen>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1203/capture-1203.mts`;
  `audit_evidence.py check` passes; the family is registered in
  `fixtures/pi-v1.3.0/baselines.json` (65 bundles / 142 artifacts / 779
  scenarios verified).
- Focused comparison selection: `ctest --preset vcpkg -LE architecture -L issue1203`,
  3/3 passed (`deliveries/1203-durable-captures.xml`), at
  `829b14edd54a2c0f4cf6b0d2d03d4ba3f4f5c2e0` (see the delivery record for the
  full commit id).
- The twelve receipts that pin the consolidation plan, the shared registry or
  the touched durable test source carry `revalidation1203` entries with their
  exact selections rerun into fresh JUnit artifacts.

## Capability status

`cch_durable:background-ownership-boundary`, `cch_durable:submission-queues`
and `cch_durable:harness-settings-defaults` advance to delivery-backed
`verified`. The other 18 rows keep their source-audit implementation status
with a registered frozen observation (`related-captured`); their C++
comparisons and whole-row receipts remain with #1180 per the ticket's own
routing.

## Remaining boundary

The 18 captured-but-not-yet-compared rows await their C++ comparisons and
receipts under #1180. The codemode rows' replay capture needs the QuickJS
guest and sqlite storage; the models capture records the frozen globals
spelling (`models.classify` / `models.generateImages`, no `models.complete`).
The bash tool truncation observation carries the random temp spill path under
a recorded normalization.

## 2026-11-06 disposition (#1229)

Retro-review blocker #1229 reconciled this receipt's evidence integrity. The
3 claims whose `discriminatingCase.test` string-equals one of the same
row's `comparisons[].test` entries now carry a non-empty `remainingBoundary`
entry in `1058-deliveries.json`: each claim stays validated but bounded and no
longer promotes its row, superseding the intermediate #1236 per-claim
withdrawal record for this receipt (removed; preserved in git history). The
rows revert to their source-audit status until a genuinely discriminating
negative/control/boundary case is delivered. No cited test, pinned input,
scenario mapping or routing digest changed; see
[1229-withdrawn-1209-root-tui-lifecycle-claims.json](1229-withdrawn-1209-root-tui-lifecycle-claims.json)
for receipts that also carry a #1229 withdrawal.
