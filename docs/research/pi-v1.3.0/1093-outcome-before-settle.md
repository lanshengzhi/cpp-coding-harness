# #1093 deterministic run outcome-before-settle

## Scope

Resolve the remaining #1093 clause reopened by the #1044 reconciliation: the
public deterministic run exposes `finished` once its outcome is decided, before
its result settles, and finished runs silently ignore later answers. The frozen
baseline is **pi-v1.3.0 / 71023aec1546d6d677a65669c5665c0927ce0dd0**. This change
preserves the delivered deterministic replay, sequence ordering, virtual time,
seed and fingerprint checks, and rejection behavior.

## Frozen evidence

`audit-1093/outcome-settle.json` captures three public observations from the
clean frozen pi checkout:

- while a host call waits, `finished` is false; after caller abort decides the
  outcome, it is true before the result promise's continuation runs; a late
  answer is silently ignored and the result remains aborted;
- an already-aborted signal makes the run finished before `start` returns,
  without VM output or host calls;
- on normal completion, `finished` is false while a host call waits and true
  when the result continuation runs; a late answer is silently ignored.

The frozen `deliver()` returns JavaScript `undefined` for a silent late answer;
the C++ public API represents the same no-op as an empty `std::optional`
rejection. The capture records this normalization. The settle witness observes
the result continuation rather than promise resolution itself, since a promise
may be resolved before its queued continuation runs.

The acceptance probe places its check at the public `AsyncResult` completion:
the callback reads `Run::finished()` synchronously as the result settles. A
local inverted-order probe that invoked completion before publishing
`finished` failed the abort case. Pre-requested cancellation now publishes the
aborted outcome and `finished` in `DeterministicRun::Impl` construction, before
the public `start()` returns.

## Delivered behavior

`DeterministicRun::Impl::finish()` publishes `finished` and the terminal outcome
under the same mutex before invoking the stored result completion. A run created
with a stop token that is already requested publishes its aborted outcome,
`finished` and `outcome_ready` synchronously in construction; `result()` then
consumes that ready outcome. Late delivery remains a silent no-op.

Three tests in `tests/codemode/CodemodeOwnerTest.cpp` load and check the captured
scenarios through the public `CodemodeSandbox::Run` API. The abort case includes
a discriminating late successful answer and proves it cannot replace the abort.
The normal case answers a waiting call, observes `finished` at result
settlement, and checks the late-answer no-op. The pre-aborted case checks
`finished` before result consumption and the aborted result. The completion
witness owns no strong reference cycle: it carries a weak run handle, while the
test keeps the run alive through settlement.

No capability-ledger status was promoted. The three new captured scenarios are
registered as explicit consolidation orphans rather than attached to an
unrelated whole-capability row.

## Validation

### Original ticket branch (historical)

The observations below belong to the original ticket branch. The integration
validation section records checks against the repaired main tree.

- Frozen capture and registration: `audit_evidence.py verify --baseline pi-v1.3.0` passed with **29 bundles, 84 artifacts and 491 scenarios**.
- Focused `ctest --preset vcpkg -L issue1093`: **14/14 passed**.
- Codemode selection: **94/94 passed**.
- Full build: `cmake --build --preset vcpkg` passed; Parity Architecture Gate passed.
- Consolidation `build` and `check`: passed with **1,464 rows, 488 scenarios, 86 explicit orphans and zero uncovered scenarios**. Named-evidence and consolidation-boundary tests passed **3/3**.
- Full Validation: the first final-tree run had one intermittent unrelated #697 Process Terminal failure; #697 passed alone, and the subsequent raw unfiltered CTest command passed **3,779 tests**, with **one expected Native TUI differential skip** (3,780 selected).
- Existing #1101 receipt selection: **14/14 passed**. Its refreshed JUnit result is [1093-registration-1101.xml](deliveries/1093-registration-1101.xml); its selected test set, input mapping and capability status are unchanged. The consolidation-plan digest and validation result were refreshed because the plan now registers the three audit-1093 scenarios.
- `scripts/format-check.sh bc35299b96eab58ce36b29bf991939fbcea0ca0e` and `git diff --check`: clean.

### Repaired main integration

The missing delivery record was restored from ticket commit `096c03428`; the
linked JUnit files below were rerun on the repaired integration inputs rather
than copied as historical passes. No new whole-capability receipt is claimed:
the original ticket refreshed the existing #1101 receipt.

- Public outcome/settlement selection: **14/14 passed**; current result:
  [1093-outcome-before-settle.xml](deliveries/1093-outcome-before-settle.xml).
- Refreshed receipt selections: #1101 **14/14**, #1090 **10/10**, #1104 **12/12**.
  The #1101 receipt now points at
  [1093-registration-1101.xml](deliveries/1093-registration-1101.xml). Source,
  test, support-header and result digests were recomputed without changing any
  scenario mapping or capability status.
- Recursive watch correction: **6/6 issue1156 cases passed**; the renamed nested
  path test failed the reviewed implementation before passing the correction.
  Move-out and hidden/name-excluded destinations no longer report stale paths;
  ordinary child deletion preserves coverage and root deletion is terminal.
- Full incremental build and build-phase Parity Architecture Gate passed.
- Full Validation with `PI_CHECKOUT=/tmp/cch-1168-pi-frozen ctest --preset vcpkg`:
  **4,015 selected / 4,014 passed / 0 failed / 1 skipped**, exit **0**; the
  architecture selection contributes **43/43 passed**. The only skip is the
  opt-in Native TUI differential capture. The frozen checkout is clean at the
  named revision. An earlier default-environment run failed named-evidence
  capture because the sibling pi checkout lacked its esbuild platform package
  and skipped six interoperability cases; the final full run passes all seven
  affected cases using the verified frozen checkout.
- Final two-axis review of the corrective patch: **Standards 0 violations /
  0 smells; Spec 0 findings**.
- Hidden #1167 RED probe retains exit **42**, **2 cases / 8 assertions**, with
  **6 passed / 2 deliberately failed** and the **3 != 1** thread-leak witness
  after replacing throwing integer parsing with `std::from_chars`.
- Frozen evidence verifier: **44 bundles / 100 artifacts / 561 scenarios**.
  Consolidation check: **1,464 rows / 558 scenarios / 140 explicit orphans /
  zero uncovered**; all seven delivered rows retain their status.
- Format check against `origin/main` and `git diff --check`: clean.

## Capability status

The deterministic-run capability row and all other capability rows retain their
existing membership, implementation and verification statuses. This bounded
change resolves the remaining public outcome/settlement ordering clause without
claiming verification of the broader deterministic-run capability row.

## Remaining boundary

Issue #1093 remains open as directed. This record does not close #1044, change
other deterministic-run semantics, or make claims about power-loss durability,
real terminal behavior or external provider access.
