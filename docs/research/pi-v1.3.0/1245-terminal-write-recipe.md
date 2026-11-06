# #1245 terminal-write recipe, 2026-11-06

## Scope and result

`cch_tui:ProcessTerminal.clearScreen` is delivery-backed verified by
`1245-terminal-clear`. The registered `process-clear-screen` capture records
`ESC[2J ESC[H`. Its comparison checks the real PTY bytes. The independent paired
case feeds those actual bytes into `VirtualTerminal` and checks that scrollback
was not cleared, then resizes a main screen and checks that the redraw clears
both screen and scrollback. The case rejects both a direct clear that adds
`ESC[3J` and a redraw that omits it. The owner selected this discriminator in
#1245 on 2026-11-06; the pre-start admission assertion was rejected there.

`clearLine`, `clearFromCursor` and signed `moveBy` have no identified direct
C++ terminal equivalent or caller seam. That design decision remains outside
this ticket.

## Recipe

1. Read the capture and the comparison test. Select a second observable case
   that rejects a plausible shallow implementation. Here, add
   `tests/tui/ProcessTerminalTest.cpp`: the paired test invokes the real
   ProcessTerminal through a PTY, passes its bytes to the virtual terminal's
   `write`, and checks the two flags before and after a main-screen redraw.
2. Edit only the `ProcessTerminal.clearScreen` row of
   `docs/research/pi-v1.3.0/1050-toolkit-capability-ledger.json`: change
   `scenario`, `evidence.artifact`, `evidence.scenario`, `evidence.result`,
   `gaps`, `notes`, and `comparisonCase` to the dated named capture and
   re-audit. The generic terminal IO bucket is retained on other rows.
   The existing `scenarioLinks` entry in
   `fixtures/pi-v1.3.0/consolidation-1058/consolidation-plan.json` already
   routes the capture; no route edit is needed. Commit these inputs before
   collecting the receipt's JUnit, so `testedRevision` contains them.
3. Measure receipt pins, rather than guessing: inspect each record's `inputs`
   in `docs/research/pi-v1.3.0/1058-deliveries.json`. The modified 1050 ledger
   is pinned by one receipt, `1201-terminal-primitives`; the test source is
   pinned by none. Promotion changes `1182-tui-route-batches.json`, pinned by
   one #1182 receipt. The registry is pinned by 22 receipts but did not change.
   Rerun the nine `issue1201` cases, review their unchanged routes, update the
   ledger input digest in that receipt, and record `revalidation1245` with its
   command, tested revision, timestamp, count and JUnit path/digest. The new
   JUnit is `deliveries/1201-terminal-primitives-revalidation-1245.xml`.
4. Run the two comparison/discriminator cases and store
   `deliveries/1245-terminal-clear.xml`. Add a record in
   `1058-deliveries.json` with `id`, `ticket`, full `implementationCommits`,
   `report`, `routingSha256`, and input `path`/`sha256` pairs. Its `validation`
   records the command, `testedRevision`, `recordedAt`, selected count, and
   JUnit `path`/`sha256`. Each `rows` claim needs `id`, `comparisons` containing
   `artifact`, `scenario`, `test`, `testSource`, a `discriminatingCase` with
   `test`, `testSource`, `reason`, and an explicit empty `remainingBoundary`.
   Pin the cited test and relevant production and evidence inputs. Compute
   `routingSha256` from the sorted row/scenario/artifact/digest mapping using
   the compact sorted-key JSON algorithm in the consolidation README.
5. Refresh `ticketStates` in
   `fixtures/pi-v1.3.0/consolidation-1058/1239-stale-fields.json` from complete
   open and closed issue lists (58 and 1098 respectively at this capture).
   This added #1244 closed and #1245 open and updated #1239 to closed. Build
   consolidation; it writes `1058-consolidated-capability-ledger.json` and
   `1058-consolidation-frontier.md`. Update the delivery-count tripwire in
   `tests/evidence/ConsolidationAuditTest.py` from 565 to 566. Rebuild
   `1182-tui-route-batches.json`, commit it, rerun the 13 `issue1182` cases,
   and update its receipt's index digest and `revalidation1245` metadata. Check
   the generated row and run the gates below.

The earlier #1245 partial delivery corrected comments in
`src/tui/include/cch/tui/Terminal.hpp`, `src/tui/ProcessTerminal.cpp`, and
`src/tui/include/cch/tui/VirtualTerminal.hpp`, with frozen citations for the
separate clear/home and redraw scrollback sequences.

A further row with an existing capture and an independent observable case costs
one row-specific re-audit, a comparison and discriminating test, one receipt,
and the measured revalidation set for its changed inputs. For the same 1050
ledger, TUI route index and an otherwise unpinned test source this is one
nine-case selection, one 13-case selection, the new row's selection and shared
gates; new captures or
shared pinned sources can increase it. Nine remaining rows therefore require
nine independently reviewed mappings and cases, not a defensible fixed time
estimate.

### Commands and observations

The previous partial lane's validation is recorded in issue #1245 and commit
`17cbbbe40`; this continuation ran the substantive commands below in order.
The initial exploratory `rg` used a malformed regex twice. `cmake --preset`
was necessary for the new test include. The first receipt script failed before
writing because CTest placed relative JUnit paths beneath `build/`; copying
those files to the recorded paths fixed it. The first consolidation build
reported #1245 missing from the recorded issue states; after refreshing them,
build and check passed. The first standalone audit test found the 565 count
tripwire; it was updated to 566. An initial full run found the stale TUI route
index (one test failed and 11 skipped); rebuilding it fixed the focused failure.
An earlier full run was interrupted after a receipt pin changed during the run.

```sh
git status --short
scripts/dev/lane-lock.sh status
scripts/dev/lane-lock.sh acquire lane-1245
cmake --preset vcpkg
cmake --build --preset vcpkg --target cch_tests_tui
CCH_LANE_NAME=lane-1245 ctest --preset vcpkg -R '^Direct clear preserves scrollback while main-screen redraw clears it$' --output-on-failure
scripts/format-check.sh HEAD
git diff --check
git add tests/tui/ProcessTerminalTest.cpp docs/research/pi-v1.3.0/1050-toolkit-capability-ledger.json
git commit -m 'Test terminal clear and re-audit its scenario (#1245)'
CCH_LANE_NAME=lane-1245 ctest --preset vcpkg -LE architecture -L issue1201 --output-junit docs/research/pi-v1.3.0/deliveries/1201-terminal-primitives-revalidation-1245.xml -j 1
CCH_LANE_NAME=lane-1245 ctest --preset vcpkg -R '^(Direct clear preserves scrollback while main-screen redraw clears it|Process Terminal clear screen emits the pi-exact clear and home bytes)$' --output-junit docs/research/pi-v1.3.0/deliveries/1245-terminal-clear.xml -j 1
cp build/docs/research/pi-v1.3.0/deliveries/*1245*.xml docs/research/pi-v1.3.0/deliveries/
gh issue list --state open --limit 2000 --json number
gh issue list --state closed --limit 2000 --json number
CCH_LANE_NAME=lane-1245 python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py build
CCH_LANE_NAME=lane-1245 python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check
CCH_LANE_NAME=lane-1245 python3 tests/evidence/ConsolidationAuditTest.py
python3 scripts/evidence/receipt_boundary.py
python3 scripts/evidence/audit_evidence.py verify
scripts/format-check.sh 17cbbbe40
git diff --check
cmake --build --preset vcpkg
TERM_PROGRAM=herdr CCH_LANE_NAME=lane-1245 ctest --preset vcpkg
python3 fixtures/pi-v1.3.0/consolidation-1058/tui_route_batches.py build
python3 tests/evidence/TuiReconciliationTest.py
git add docs/research/pi-v1.3.0/1182-tui-route-batches.json tests/evidence/ConsolidationAuditTest.py
git commit -m 'Refresh TUI route index for terminal clear (#1245)'
TERM_PROGRAM=herdr CCH_LANE_NAME=lane-1245 ctest --preset vcpkg -L issue1182 --output-junit docs/research/pi-v1.3.0/deliveries/1182-tui-route-revalidation-1245.xml
cp build/docs/research/pi-v1.3.0/deliveries/1182-tui-route-revalidation-1245.xml docs/research/pi-v1.3.0/deliveries/
CCH_LANE_NAME=lane-1245 python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py build
CCH_LANE_NAME=lane-1245 python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check
cmake --build --preset vcpkg
TERM_PROGRAM=herdr CCH_LANE_NAME=lane-1245 ctest --preset vcpkg
```

The focused case passed; the receipt selections passed 9/9, 13/13 and 2/2.
The route-batch test passed 11/11.
Consolidation check reported 1464 rows, 566 delivery-backed rows and zero
uncovered scenarios. Receipt boundary passed 32 receipts, 808 claims and 966
comparisons. Frozen evidence verification remained at 84 bundles, 180
artifacts and 1012 scenarios. Full Validation completed in 245 seconds: 4560
selected, 4549 passed and 11 skipped. Format and diff checks passed. The
source-audit comparison against `17cbbbe40` changed one row and left 42 other
rows with the historical terminal IO scenario.

### Review corrections

The subsequent #1245 review required real scrollback content, a frozen source
citation, and correctly rotated validation records. `ProcessTerminalTest.cpp`
now seeds scrollback before both halves, checks the cells after the PTY output
and the redraw, and exercises `VirtualTerminal::clear_screen()` as well. The
1050 row's `evidence.result` cites `packages/tui/src/terminal.ts:567-569`, and
`evidence.route` identifies the observed clear and home. No other source row
changed. Commit `51c90d0` contains both pinned inputs.

The measured pin set for these two changed inputs is `1201-terminal-primitives`
(1050 ledger) and `1245-terminal-clear` (ledger and test). The previously changed
route index remains pinned by the #1182 receipt. In `1058-deliveries.json`, the
#1182 and #1201 `revalidation1245` entries retain `reason`, `changedInputs`,
and `previousValidation`, with their earlier #1245 results rotated into primary
`validation`. The fresh #1201 and #1245 runs each rotate primary `validation`
again and record `revalidation1245Review` with the changed paths and previous
validation. The refreshed input digests, JUnit paths and hashes are recorded in
those receipts. The same second-row cost estimate above remains applicable.

The continuation ran the following validation commands after acquiring lane
`issue-1245-review` and committing the two changed inputs. The receipt selections
were also run once with temporary output paths before the recorded JUnit runs.

```sh
cmake --build --preset vcpkg --target cch_tests_tui
CCH_LANE_NAME=issue-1245-review ctest --preset vcpkg -R '^Direct clear preserves scrollback while main-screen redraw clears it$' --output-on-failure
CCH_LANE_NAME=issue-1245-review ctest --preset vcpkg -LE architecture -L issue1201 --output-junit "$PWD/docs/research/pi-v1.3.0/deliveries/1201-terminal-primitives-revalidation-1245-review.xml" -j 1
CCH_LANE_NAME=issue-1245-review ctest --preset vcpkg -R '^(Direct clear preserves scrollback while main-screen redraw clears it|Process Terminal clear screen emits the pi-exact clear and home bytes)$' --output-junit "$PWD/docs/research/pi-v1.3.0/deliveries/1245-terminal-clear-review.xml" -j 1
CCH_LANE_NAME=issue-1245-review python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py build
CCH_LANE_NAME=issue-1245-review python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check
CCH_LANE_NAME=issue-1245-review python3 tests/evidence/ConsolidationAuditTest.py
python3 scripts/evidence/receipt_boundary.py
python3 scripts/evidence/audit_evidence.py verify
python3 tests/evidence/TuiReconciliationTest.py
scripts/format-check.sh 80ad9fd58
git diff --check
cmake --build --preset vcpkg
TERM_PROGRAM=herdr CCH_LANE_NAME=issue-1245-review ctest --preset vcpkg
```

The focused test and receipt runs passed (1, 9 and 2 cases). Consolidation
passed with 566 delivery-backed rows, the audit suite passed 91 tests, the route
suite passed 11 tests, the receipt boundary passed 32 receipts and frozen
evidence remained at 84 bundles, 180 artifacts and 1012 scenarios. Full
Validation passed with 4560 selected, 4549 passed and 11 skipped in 251.58
seconds; format and diff checks were clean.
