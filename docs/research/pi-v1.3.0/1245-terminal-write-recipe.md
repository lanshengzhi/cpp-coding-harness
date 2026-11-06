# #1245 terminal-write recipe: partial probe, 2026-11-06

## Scope and blocker

`cch_tui:ProcessTerminal.clearScreen` remains implemented and unverified; zero
rows were promoted. The registered `audit-1244-terminal-writes/terminal-writes.json`
`process-clear-screen` scenario captures `ESC[2J ESC[H`. The existing PTY test
compares those exact bytes and also rejects `clear_screen()` before Pike starts.
The latter tests Pike's lifecycle admission, while the frozen pi operation is a
parameterless direct stdout write. Whether that local lifecycle assertion can
serve as the delivery contract's independent parity discriminator needs an owner
ruling, or a different negative/boundary scenario captured and compared at an
appropriate seam. No such ruling or additional capture was established here.
The blocked step is the discriminating case in the receipt; promoting on the
happy-path byte comparison alone would violate #1245. The historical
`E02/E09/E12 terminal IO` obligation also remains in place; no source audit,
route, receipt or capability row was changed.

`clearLine`, `clearFromCursor` and signed `moveBy` have no identified equivalent
caller seam; resolving it is outside this ticket.

## Recipe

This is a partial recipe, not a reproducible promotion: the discriminator,
row-specific re-audit and delivery receipt remain to be designed and executed.
The observed path is:

1. Inspect the frozen `terminal.ts:567-569` operation and the registered
   artifact; compare via `tests/tui/ProcessTerminalTest.cpp`. Confirm a separate
   discriminating test and explain why it catches a plausible shallow defect.
2. Re-audit only `ProcessTerminal.clearScreen` in
   `docs/research/pi-v1.3.0/1050-toolkit-capability-ledger.json`: its historical
   scenario fields and `comparisonCase` currently name the generic bucket. Use
   the named captured scenario with dated row-specific justification; preserve
   the bucket on every other row. Inspect the plan's `scenarioLinks`
   route before changing it, so the resulting unique obligation is correct.
3. Measure pins in `docs/research/pi-v1.3.0/1058-deliveries.json` before edits.
   This checkout has one receipt pinning the 1050 ledger:
   `1201-terminal-primitives` (its last selection was 9 cases). The registry
   `fixtures/pi-v1.3.0/baselines.json` is pinned by 22 receipts, including that
   one; their union is 22. A new or changed capture would require reviewing and
   rerunning those affected receipt selections, updating only reviewed changed
   digests and recording revalidation metadata. No registry change is needed
   merely to reuse the existing registered capture. The three comment-only
   source files below are pinned by zero current 1058 receipt `inputs`; the
   separate #1182 evidence selection must also be reviewed if its pinned inputs
   change. These are measurements of current direct input paths, not a claim
   that every future edit has the same dependency set.
4. Record a receipt in `1058-deliveries.json`: record `id`, `ticket`, full
   `implementationCommits`, `report`, `routingSha256`, and `inputs` path/digest
   pairs; `validation` carries the exact command, `testedRevision`, UTC
   `recordedAt`, selected count, and JUnit path/digest. Each `rows` entry needs
   `id`, `comparisons` with `artifact`, `scenario`, `test`, `testSource`, a
   `discriminatingCase` with `test`, `testSource`, `reason`, and an explicitly
   empty `remainingBoundary`. The compared and discriminating cases must pass,
   the test sources must be pinned, and `receipt_boundary.py` must anchor the
   cited artifact in the source. See `1058-delivery-reconciliation.md` and
   `fixtures/pi-v1.3.0/consolidation-1058/README.md` for the full contract.
5. Build and check consolidation, test its rejection boundaries, verify frozen
   evidence and receipt boundaries, run Full Validation and format/diff checks.
   Record actual selections and results, then review before delivery.

A second row's cost cannot yet be estimated from a completed promotion. At
minimum it needs its own frozen comparison and discriminating case, one
row-specific re-audit and receipt, plus the selections of the receipts pinning
its changed inputs. Nine remaining rows therefore have no defensible total
cost estimate from this partial probe.

### Edits made

- `src/tui/include/cch/tui/Terminal.hpp`: `clear_screen` comment separates the
  clear/home operation from the redraw's scrollback clear.
- `src/tui/ProcessTerminal.cpp`: `clear_screen` comment describes the same split
  and when local bookkeeping resets.
- `src/tui/include/cch/tui/VirtualTerminal.hpp`: scrollback flag comment
  describes the separate sequence.
- This file: blocker, measured dependency set and partial recipe.

### Validation commands and results

Commands below are in execution order for the substantive checks; exploratory
`rg` and file reads are omitted. `lane-1245` was acquired after status showed
it free. The first attempt at the consolidation script used the nonexistent
`scripts/evidence/consolidate_audits.py` path and failed. `audit_evidence.py
verify` passed (84 bundles, 180 artifacts, 1012 scenarios);
`receipt_boundary.py` passed (31 receipts, 807 claims, 965 comparisons).
The initial `ConsolidationAuditTest.py` invocation omitted `CCH_LANE_NAME` and
failed its lock-sensitive cases. `scripts/format-check.sh HEAD` passed.

```sh
CCH_LANE_NAME=lane-1245 python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py build
CCH_LANE_NAME=lane-1245 python3 fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py check
CCH_LANE_NAME=lane-1245 python3 tests/evidence/ConsolidationAuditTest.py
cmake --build --preset vcpkg
TERM_PROGRAM=herdr CCH_LANE_NAME=lane-1245 ctest --preset vcpkg
```

Build/check passed with 1464 rows and zero uncovered scenarios. The first
identity-correct test run overlapped the ledger writer and encountered an empty
file; the sequential rerun passed all 91 tests. The first 60-second incremental
build timed out; the subsequent build completed. The full CTest run selected
4559: 4548 passed, 11 skipped, zero failed. These checks do not resolve the
receipt blocker. The untracked `.lane-notes/` directory predated this work.
