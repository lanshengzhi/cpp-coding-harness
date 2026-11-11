# Test temporary root: the twelve rebound receipts rerun (2026-11-11)

## Scope

Close-out follow-up to
[1232-test-tmp-root-install-diagnostics-2026-11-11](1232-test-tmp-root-install-diagnostics-2026-11-11.md).
That change edited `tests/Catch2Main.cpp`, so thirteen delivery receipts whose
`inputs` pin that file had to be reconciled under the
[#1044 delivery evidence contract](1058-delivery-reconciliation.md#delivery-evidence-contract).
Receipt `1101-structured-content` reran its exact 36-case selection at
`d34fc1b30`; the other twelve only had the pinned input digest refreshed and
carried a `revalidationTestTmpRoot` note declaring a harness-only revalidation,
keeping their previous JUnit files.

A rebound digest with an unchanged JUnit leaves the receipt in exactly the state
the contract exists to prevent: the recorded `selected` bytes are not the bytes
the JUnit was produced from. This record removes that deviation by rerunning
each of the twelve selections on a build that contains the new
`tests/Catch2Main.cpp`, and by replacing the harness-only note with the measured
rerun.

## Delivered behavior

- Selection audit: `docs/research/pi-v1.3.0/1058-deliveries.json` carries
  thirteen records whose `inputs` name `tests/Catch2Main.cpp` (the assumed
  twelve plus the already rerun `1101-structured-content`), so the rerun set is
  exactly twelve: `1118-codemode-catalog`, `1176-agent-comparisons`,
  `1177-application-comparisons`, `1178-chord-comparisons`,
  `1179-codemode-comparisons`, `1181-mcp-comparisons`, `1182-tui-comparisons`,
  `1184-chord-context-cancellation`, `1185-chord-delta-ingestion`,
  `1186-chord-typed-json`, `1187-chord-progress-pacing` and
  `1188-chord-observer-serialization`. Their digests were already rebound at
  `d34fc1b30` and their JUnit files were still the pre-change artifacts, which
  is the deviation this lane closes.
- Each receipt's `validation.command` was read back from the receipt itself and
  its selector was reused verbatim — `-L` label, `-R` expression and `-j` value,
  with only the `--output-junit` path repointed at the new artifact. No selector
  was retyped from memory, widened or narrowed: the twelve selections stay
  byte-identical apart from the artifact name.
- Before each run, `ctest --test-dir <abs build> -N` on the same selector
  reproduced the receipt's recorded `selected` count exactly (12/12), so no run
  could silently cover a different case set.
- All twelve selections passed on the rerun: 544 cases selected, 544 passed,
  0 skipped, 0 failed. No receipt was rebound from a failing result.
- Receipt updates: top-level `validation` gains the new `junit` path and digest,
  the actual `selected`, `testedRevision` `2ea9ae1c360f2a3221fd1dcf5326bf2ac471627a`
  (the checkout the runs happened on) and the run's `recordedAt`. Its `command`
  now names the new artifact. `rows`, `routingSha256`, `implementationCommits`
  and `report` are untouched; inputs were not rehashed.
- `revalidationTestTmpRoot` is rewritten to describe the rerun with its measured
  counts. The previous harness-only text is retained verbatim as
  `supersededHarnessOnlyRebind` (with its `d34fc1b30` provenance and the artifact
  that supersedes it), and the pre-rerun validation block is retained as
  `previousValidation`, so the rebound-digest history is visible rather than
  erased. Earlier `previousValidation` chains under the records' other
  revalidation entries are untouched.
- The twelve superseded JUnit artifacts remain in `deliveries/` unreferenced;
  the new artifacts are the receipts' evidence.

## Validation

Checkout `2ea9ae1c360f2a3221fd1dcf5326bf2ac471627a` on
`t3/test-tmp-root-install-diagnostics`, lane `test-tmp-root-receipts`, absolute
build tree `build/` (Debug `vcpkg` preset). `cmake --build --preset vcpkg`
reported only the build-phase Parity Architecture Gate (1/1) because the tree
already contained the new harness; the test binaries were current. Every run
used `--test-dir /home/lansy/Work/github/coding-agent/cpp-coding-harness/build`,
`--no-tests=error`, `--output-on-failure`, the receipt's own `-j` value,
`TERM_PROGRAM=herdr` and `CCH_LANE_NAME=test-tmp-root-receipts`.

| Receipt | Selection (from `validation.command`) | Jobs | Selected | Passed | Skipped | Failed | Wall | New artifact |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `1118-codemode-catalog` | `-L issue1118` | `-j 1` | 19 | 19 | 0 | 0 | 7.28 s | `1118-codemode-catalog-revalidation-test-tmp-root.xml` |
| `1178-chord-comparisons` | `-L chord` | `-j 1` | 104 | 104 | 0 | 0 | 0.58 s | `1178-chord-comparisons-revalidation-test-tmp-root.xml` |
| `1181-mcp-comparisons` | `-R` (16 named cases) | `-j 1` | 16 | 16 | 0 | 0 | 3.51 s | `1181-mcp-comparisons-revalidation-test-tmp-root.xml` |
| `1185-chord-delta-ingestion` | `-L chord` | `-j 1` | 104 | 104 | 0 | 0 | 0.57 s | `1185-chord-delta-ingestion-revalidation-test-tmp-root.xml` |
| `1184-chord-context-cancellation` | `-L issue1184` | `-j 1` | 10 | 10 | 0 | 0 | 0.21 s | `1184-chord-context-cancellation-revalidation-test-tmp-root.xml` |
| `1186-chord-typed-json` | `-L chord` | `-j 1` | 104 | 104 | 0 | 0 | 0.58 s | `1186-chord-typed-json-revalidation-test-tmp-root.xml` |
| `1188-chord-observer-serialization` | `-L chord` | `-j 1` | 104 | 104 | 0 | 0 | 0.58 s | `1188-chord-observer-serialization-revalidation-test-tmp-root.xml` |
| `1187-chord-progress-pacing` | `-L issue1187` | `-j 1` | 4 | 4 | 0 | 0 | 0.16 s | `1187-chord-progress-pacing-revalidation-test-tmp-root.xml` |
| `1182-tui-comparisons` | `-L issue1182` | `-j 1` | 13 | 13 | 0 | 0 | 8.12 s | `1182-tui-comparisons-revalidation-test-tmp-root.xml` |
| `1179-codemode-comparisons` | `-R` (52 named cases) | `-j 1` | 52 | 52 | 0 | 0 | 107.40 s | `1179-codemode-comparisons-revalidation-test-tmp-root.xml` |
| `1176-agent-comparisons` | `-R` (5 named cases) | `-j 1` | 5 | 5 | 0 | 0 | 0.17 s | `1176-agent-comparisons-revalidation-test-tmp-root.xml` |
| `1177-application-comparisons` | `-R` (9 named cases) | `-j 1` | 9 | 9 | 0 | 0 | 2.81 s | `1177-application-comparisons-revalidation-test-tmp-root.xml` |

Totals: 544 selected, 544 passed, 0 skipped, 0 failed, 131.97 s of CTest wall
time. The four `-R` expressions (1116, 4422, 384 and 776 characters) are stored
verbatim in each receipt's `validation.command` and were passed to CTest
unmodified; the artifacts list the case names each one matched, and each case
count equals the receipt's `selected`. Every artifact's `<testsuite tests>`
attribute equals its `selected` and its receipts' `junit.sha256` is the digest of
the committed file.

Reconciliation gates:

- `CCH_LANE_NAME=test-tmp-root-receipts python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py build`
  wrote `1058-consolidated-capability-ledger.json` (5,876,034 bytes, 1464 rows).
- `consolidate_audits.py check` exited 0: 1009 scenarios, 591 named by a row, 87
  covered by a referenced artifact, 331 explicit orphans, 0 uncovered; 14
  residual trackers; 38 relative links resolve.
- `tests/evidence/ConsolidationAuditTest.py` exited 0 (94 tests, 32.97 s).
- `python3 scripts/evidence/audit_evidence.py verify --baseline pi-v1.3.0`
  exited 0: 84 bundles, 180 artifacts, 1012 scenarios at baseline revision
  `71023aec1546d6d677a65669c5665c0927ce0dd0`.
- `scripts/format-check.sh d775b8fe8` reported clean; `git diff --cached --check`
  is clean and no build output is staged.

## Capability status

No capability row, comparison scenario, discriminator or routing digest changes.
The twelve receipts again carry a JUnit file produced from the exact bytes their
`inputs` pin, so their existing claims rest on real reruns instead of a
harness-only digest refresh.

## Remaining boundary

None for this deviation. The receipts' other `previousValidation` chains still
record their own earlier rebases, and the twelve superseded JUnit artifacts
remain on disk as history rather than as evidence.
