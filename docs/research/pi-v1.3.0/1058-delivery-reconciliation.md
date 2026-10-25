# #1044 delivery reconciliation and capability verification

## Scope

Reconcile the already merged #1101 and #1071 work, correct stale frontier facts,
and enable evidence-backed whole-row verification. The source audit ledgers,
frozen captures, capability membership, Magpie/XDG decisions and #1044's final
acceptance obligations remain unchanged.

The 32 drafts in the #1058 frontier are planning records. GitHub's 76
implementation slices and their native dependencies determine the live work
frontier. A closed slice does not necessarily verify an entire capability row.

## Delivered behavior

- [#1101](1101-structured-content.md) reconciles the MCP structured-result
  implementation and adds a complete replay of the original frozen audit
  obligations through the public MCP client and real application tool wrapper.
- [#1071](1071-legacy-retirement.md) reconciles the retirement of the unconsumed
  agent-side SQLite/task subsystem and checks the remaining independent callers
  and configured dependency closure. It makes no new parity claim.
- [1058-deliveries.json](1058-deliveries.json) is the reviewed delivery input to
  the deterministic consolidation. Source audit records are retained in
  `sourceRows`; each promoted row also keeps `auditStatus` and names its delivery.
- The frontier's close-out distinguishes implemented packages from outstanding
  behavior and comparison work. The eight residual TUI issues are recorded as
  closed without promoting their capability rows. Existing dated PTY evidence
  does not establish real emulator rendering.

## Delivery evidence contract

A record selects one ticket and its full implementation commit IDs, a repository
report, and the SHA-256 digests of the relevant production, test, build and fixture
inputs. `testedRevision` records the checkout base; the input digests bind the
selected files actually tested, including any changes made during reconciliation.
The record pins the actual focused CTest `--output-junit` artifact and records the
command, UTC capture time and selected count.

Every claimed row must be supported and map **each of its exact audit comparison
scenarios** to a registered frozen artifact and a passing CTest case whose source
is pinned. A passing discriminating case and its reason are required. The remaining
boundary list must be explicitly empty. Missing cases, skipped/failed cases,
duplicate claims, unknown rows/scenarios, wrong baseline/revision and changed
selected source/test/result bytes all reject the delivery. No record can alter
membership or promote an adjacent row.

The shared named-evidence verifier checks the entire upstream provenance and
integrity chain before a delivery is used. Stored commands are descriptive and
are never executed by consolidation. Both `build` and `check` validate deliveries;
a failed proof cannot leave a newly generated success artifact.

The automatic checks bind evidence and reject common false promotions. Review
must still establish that the selected input set is sufficient, that each named
C++ case actually compares the selected frozen scenario, and that the entire
capability is covered. A green JUnit file, a hash, an existing symbol or a closed
ticket alone establishes none of those properties. Changes outside a record's
selected inputs require a review of whether that record remains applicable.

For a future delivery:

1. Run and review the C++ comparison and a discriminating case against registered
   frozen scenarios. Record remaining obligations honestly; partial delivery stays
   in its ticket/report and does not verify a whole row.
2. Build the relevant targets, inspect `ctest --preset vcpkg -N` for the exact
   selection, and run it with `--output-junit` into a committed delivery artifact.
3. Add the reviewed scenario-to-case mapping, measured count and file/result
   digests to `1058-deliveries.json`; do not edit a source audit to manufacture
   verification. A source-only row needs explicit comparison scenarios in the
   consolidation plan before it can have a complete receipt.
4. Run `consolidate_audits.py build`, `consolidate_audits.py check`, the
   consolidation boundary tests, and the repository's required validation tier.
5. Record the result in the ticket's owning report and follow the existing issue
   completion procedure. Leave #1044 open while any included obligation remains.

## Validation

Validation on 2026-10-25 used checkout base
`2c6c4b9401051ac1d8cb59e17e443d74e0627318` plus the selected working-tree inputs
pinned in the #1101 receipt:

- Incremental `cch_tests_coding_agent` build and full
  `cmake --build --preset vcpkg -j4`: passed.
- #1101 selection: 14 selected, 14 passed, with the actual JUnit artifact and
  51 selected input digests in [1058-deliveries.json](1058-deliveries.json).
  The original-audit replay passed all 94 assertions.
- #1071 retained SessionStore selection: 24 selected, 24 passed. Independent
  transcript export selection: 9 selected, 9 passed. The owning report records
  the exact selections and configured core/library link evidence.
- First unfiltered `ctest --preset vcpkg`: 3717 selected, 3715 passed, 1 failed,
  1 skipped, exit 8. The already quarantined #632 Projection performance case
  measured 2.170 ms processor time against its 2 ms Debug bound. Its 172
  functional assertions passed. The same seed (`3464243709`) passed in isolation
  with all 173 assertions and 1.507 ms processor time. This is consistent with
  the documented runner sensitivity; the underlying cause was not established.
- Second unfiltered `ctest --preset vcpkg -j2`: 3717 selected, 3716 passed,
  0 failed, 1 skipped, exit 0 (224.50 seconds). No assertion or timeout was
  changed and no test label was excluded. The skipped Native TUI differential
  case explicitly requires a separately selected dual-runtime capture environment;
  it supplies no new terminal acceptance evidence here.
- All 43 names discovered by `ctest --preset vcpkg -N -L architecture` are
  present and passed in both full-run JUnit results. This is coverage within
  the unfiltered runs, not a claim of a separate architecture-only invocation.
- Consolidation `build` and `check`: passed, with 1464 source/consolidated rows,
  exactly 1 validated delivery row, 474 non-bootstrap scenarios, 72 explicit
  orphan scenarios and 0 uncovered scenarios.
- `python3 tests/evidence/ConsolidationAuditTest.py`: 39 passed, including digest,
  membership, missing/failed/skipped-result, scenario coverage and unsupported
  promotion rejection cases.
- `python3 scripts/evidence/audit_evidence.py verify`: 25 bundles, 79 artifacts
  and 477 scenarios verified at the full frozen revision.
- `scripts/format-check.sh 2c6c4b9401051ac1d8cb59e17e443d74e0627318`,
  `git diff --check` and relative report link checks: clean.

Manual standards/spec review and a comparison with the base ledger confirm that
all 1464 memberships and `sourceRows` are preserved, exactly one row changes its
implementation/verification status, and that row's `auditStatus` matches its
original consolidated facts. Review of the selected pins and frozen scenario
mapping is recorded in the #1101 report.

## Capability status

Only `cch_mcp:mcp-tools-call-structured` advances from `partial` /
`upstream-captured` to `implemented` / `verified` in this reconciliation. All
other statuses retain their existing audit/delivery state. The audit's aggregate
implementation counts are not an estimate of final project completion: the 858
historical TUI routes and broad composite durable rows still need their own
current comparison records.

## Remaining boundary

The Session migration #1068 → #1069 → #1070 remains the main application dependency
chain. Full Durable Runtime, bidirectional JSONL/SQLite continuation, remaining
Agent/MCP/application behavior, and real emulator evidence remain open under
#1044. This reconciliation neither broadens capability membership nor accepts
those remaining obligations through a full-suite pass.

The reconciliation artifacts are published on `main` in
[`8425ff3db04168a8e992e90b2304c65a8e268ff8`](https://github.com/lanshengzhi/cpp-coding-harness/commit/8425ff3db04168a8e992e90b2304c65a8e268ff8).
[#1101](https://github.com/lanshengzhi/cpp-coding-harness/issues/1101#issuecomment-6398454960)
and [#1071](https://github.com/lanshengzhi/cpp-coding-harness/issues/1071#issuecomment-6398454965)
are closed with all four acceptance criteria checked and their in-flight labels
removed. Their close-out comments map each criterion to the published implementation
and validation evidence. `scripts/verify-closed-issue.sh 1101` and
`scripts/verify-closed-issue.sh 1071` both pass. #1044 remains open; closing these
bounded slices does not accept its remaining obligations.
