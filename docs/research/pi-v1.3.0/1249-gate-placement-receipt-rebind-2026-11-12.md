# #1249 gate placement: seventeen drifted receipts rebound (2026-11-12)

## Scope

Close-out of the receipt fallout from `49f2a3a7a` ("run the build-phase Parity
Gate outside the build it gates"). That change moved the build-phase Parity
Architecture Gate out of the build it gates, so it changed eight build inputs
that `docs/research/pi-v1.3.0/1058-deliveries.json` pins. Seventeen receipts
pinned one or more of those bytes and were therefore rejected by consolidation:
1090, 1101, 1118, 1176, 1177, 1178, 1179, 1181, 1182, 1184, 1185, 1186, 1187,
1188, 1192, 1194 and 1201. This pass reruns each receipt's own recorded
selection and rebinds only the fields the delivery evidence contract names.

The eight changed inputs are `CMakeLists.txt` and
`cmake/tests/{Agent,Chord,Codemode,CodingAgent,Durable,Mcp,Tui}Tests.cmake`.
They carry 34 pinned `(receipt, path)` entries in total, which is exactly the
baseline `scripts/dev/lane-preflight.py` reported before this pass: *committed
drift 34, uncommitted drift 0, missing 0; paths: CMakeLists.txt,
cmake/tests/AgentTests.cmake, …; tickets 1090 1101 1118 1176 1177 1178 1179
1181 1182 1184 1185 1186 1187 1188 1192 1194 1201*. No other pinned input had
drifted.

What changed in those files: `cch_parity_gate_build` left `all`; the 15
per-shard `add_dependencies(<shard> ${CCH_PARITY_BUILD_GATE_TARGET})` lines and
the `pike_tests` aggregate's entry are gone; the
`cch_parity_attach_build_gate_post_build()` POST_BUILD attachment and the seven
`CCH_PARITY_BUILD_GATE_*` cache variables that only it and the target consumed
are deleted. The Gate now runs as its own build invocation (one remaining
edge), so no build edge can schedule it next to compilation.

Checkout `49f2a3a7a908f87afca254880216b74e3ff0ea45` on branch `receipts-1249`,
working tree clean at the start; Debug `vcpkg` preset already current in
`build/` (`ninja: no work to do`), lane `receipts-1249` taken with
`scripts/dev/with-lane.sh` for every build, test and evidence step.

## Delivered behavior

- Selection reuse, not retyping. For each receipt, the selector was read back
  from its own `validation.command` in `1058-deliveries.json` and passed to
  CTest verbatim. The seventeen receipts use thirteen distinct selections;
  `-L chord` covers four receipts and `-L ^(issue1090|issue1101|issue1104)$`
  covers two.
- Count check before every run. `ctest --preset vcpkg -N <selector>` on the
  build tree reported exactly the recorded `selected` count for all thirteen
  selections (36, 19, 104, 16, 10, 4, 13, 52, 5, 9, 5, 1, 9). No selection was
  widened or narrowed.
- Runs. Each selection ran with `--no-tests=error`, the record's `-j` value,
  `--output-on-failure` and `TERM_PROGRAM=herdr`, writing a new JUnit artifact
  under `deliveries/`. All thirteen passed with zero skips and zero failures:
  **283 case executions, 283 passed**.

| Records | Selector (verbatim from `validation.command`) | Selected | Passed | Skipped | Failed | Wall | Artifact |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1090, 1101 | `-L ^(issue1090\|issue1101\|issue1104)$`, `-j 1` | 36 | 36 | 0 | 0 | 2.23 s | `1090-1101-issue1090-1101-1104-revalidation-1249-gate-placement.xml` |
| 1118 | `-L issue1118`, `-j 1` | 19 | 19 | 0 | 0 | 6.55 s | `1118-codemode-catalog-revalidation-1249-gate-placement.xml` |
| 1178, 1185, 1186, 1188 | `-L chord`, `-j 1` | 104 | 104 | 0 | 0 | 0.59 s | `1178-1185-1186-1188-chord-label-revalidation-1249-gate-placement.xml` |
| 1181 | `-R ^('<16 anchored cases>')`, `-j 1` | 16 | 16 | 0 | 0 | 3.50 s | `1181-mcp-comparisons-revalidation-1249-gate-placement.xml` |
| 1184 | `-L issue1184`, `-j 1` | 10 | 10 | 0 | 0 | 0.20 s | `1184-chord-context-cancellation-revalidation-1249-gate-placement.xml` |
| 1187 | `-L issue1187`, `-j 1` | 4 | 4 | 0 | 0 | 0.15 s | `1187-chord-progress-pacing-revalidation-1249-gate-placement.xml` |
| 1182 | `-L issue1182`, `-j 1` | 13 | 13 | 0 | 0 | 8.07 s | `1182-tui-comparisons-revalidation-1249-gate-placement.xml` |
| 1179 | `-R ^('<52 anchored cases>')`, `-j 1` | 52 | 52 | 0 | 0 | 105.89 s | `1179-codemode-comparisons-revalidation-1249-gate-placement.xml` |
| 1176 | `-R ^('<5 anchored cases>')`, `-j 1` | 5 | 5 | 0 | 0 | 0.20 s | `1176-agent-comparisons-revalidation-1249-gate-placement.xml` |
| 1177 | `-R ^('<9 anchored cases>')`, `-j 1` | 9 | 9 | 0 | 0 | 2.82 s | `1177-application-comparisons-revalidation-1249-gate-placement.xml` |
| 1192 | `-LE architecture -R ^('<5 anchored cases>')`, `-j 1` | 5 | 5 | 0 | 0 | 4.80 s | `1192-codemode-tool-table-close-revalidation-1249-gate-placement.xml` |
| 1194 | `-LE architecture -L issue1194`, `-j 1` | 1 | 1 | 0 | 0 | 0.16 s | `1194-codemode-portable-entry-revalidation-1249-gate-placement.xml` |
| 1201 | `-LE architecture -L issue1201` (record carries no `-j`) | 9 | 9 | 0 | 0 | 2.05 s | `1201-terminal-primitives-revalidation-1249-gate-placement.xml` |

The anchored `-R` expressions are the receipts' own and are reproduced in full
in each record's `validation.command` and `previousValidation.command`; the
table abbreviates them only to keep the row readable.

- Shared artifacts, named per receipt. Two selections cover more than one
  receipt, so those receipts reference one artifact produced by one run:
  1178/1185/1186/1188 all reference the `-L chord` artifact, and 1090/1101 both
  reference the `issue1090|issue1101|issue1104` artifact. The case set is
  identical for every sharer (104 and 36 respectively), and each record's
  `revalidation1249GatePlacement.reason` states the sharing.
- Receipt updates, and nothing else. Each of the seventeen records gains the
  new `junit.path` and `junit.sha256`; `selected` keeps its recorded value,
  re-recorded from the measured suite (`tests` and case count); `testedRevision`
  is `49f2a3a7a908f87afca254880216b74e3ff0ea45`; `recordedAt` is
  `2026-11-12T07:20:17Z`; its `command` names the artifact the run wrote and
  records the `--output-on-failure` flag. Exactly the 34 drifted input digests
  are rebound; no other input was rehashed. `rows`, `routingSha256`,
  `implementationCommits` and `report` are byte-identical in all seventeen
  records, and no other record changed.
- History preserved. Each record gains a `revalidation1249GatePlacement` entry
  whose `changedInputs` is that record's own drifted paths (a subset of the
  eight changed inputs) and whose `previousValidation` is the pre-rerun
  validation block verbatim — old artifact path and digest, old
  `testedRevision`, old `recordedAt`. Earlier `previousValidation` chains under
  each record's other revalidation entries are untouched, and every superseded
  artifact stays in `deliveries/` as history.
- Artifact-path note, recorded because it is reproducible and was measured:
  CTest resolves a relative `--output-junit` path against the test preset's
  binary directory (`build/`), so the recorded `command` carries the absolute
  source-tree path the run used, while `junit.path` stays repository-relative
  for the verifier. The command list is descriptive; consolidation never
  executes it.

## Validation

Every command ran inside the lane lock with `CCH_LANE_NAME=receipts-1249`
exported by `scripts/dev/with-lane.sh`, and every CTest invocation carried
`TERM_PROGRAM=herdr`.

| Step | Command | rc | Measured |
| --- | --- | --- | --- |
| Incremental build | `cmake --build --preset vcpkg` | 0 | `ninja: no work to do` |
| Build-phase Gate | `cmake --build --preset vcpkg --target cch_parity_gate_build` | 0 | `[1/1] Parity Architecture Gate (build phase)` PASS |
| Count probes | `ctest --preset vcpkg -N <selector>` × 13 | 0 | every total equals the recorded `selected` |
| Receipt reruns | thirteen selections above | 0 | 283/283 passed, 0 skipped, 0 failed |
| Ledger build | `fixtures/pi-v1.3.0/consolidation-1058/consolidate_audits.py build` | 0 | 1,464 rows, ledger 5,932,493 bytes |
| Ledger check | `… consolidate_audits.py check` | 0 | 1,464 source rows into 1,464 consolidated rows; 577 delivered rows; 1,009 scenarios, 591 named by a row, 87 artifact-covered, 331 explicit orphans, 0 uncovered; 38 relative links resolve |
| Consolidation audit tests | `python3 tests/evidence/ConsolidationAuditTest.py` | 0 | 94 tests, OK (34.5 s) |
| Evidence verification | `python3 scripts/evidence/audit_evidence.py verify --baseline pi-v1.3.0` | 0 | 84 bundles, 180 artifacts, 1,012 scenarios at `71023aec…` |
| Formatting gate | `scripts/format-check.sh origin/main` | 0 | clean |
| Receipt preflight | `python3 scripts/dev/lane-preflight.py --lane receipts-1249 --revision 49f2a3a7a…` | 1 | *committed drift 0, uncommitted drift 0, missing 0* (pass); rc 1 from two `unknown` lines only, see below |
| Full suite | `TERM_PROGRAM=herdr ctest --preset vcpkg -j2` | 0 | 4,607 selected, 4,596 passed, 11 skipped, 0 failed, 425.03 s |

The preflight's two `unknown` lines are environment conditions, not receipt
state: *current root, branch, HEAD or status unavailable* because this checkout
tracks the `CLAUDE.md` symlink while `core.symlinks` is unset (so the tool
refuses to assert cleanliness rather than guess), and *upstream checkout not
supplied* because no `--checkout` was given. The delivery-receipt line is the
line this pass gates on and it is `pass` with zero drift in all three classes;
the same line read `attention: committed drift 34` before the rebind.

The full suite is unfiltered: no label was excluded. The eleven skips
self-select at test level through documented environment preconditions — nine
pi↔C++ dual-runtime continuation captures that need a separately selected
capture environment, the realtime-clock tool-duration case, and the Native TUI
differential capture. They are the same cases earlier records report as
skipped, and none of them covers a receipt rebound here.

## Capability status

No capability row, comparison scenario, discriminating case,
`implementationCommits` or routing digest changes. The ledger's `rows`,
`counts` and every other section are byte-identical to the committed ledger;
the only ledger difference is the seventeen delivery record blocks rebuilt
from the updated manifest. The seventeen receipts again pin a JUnit file
produced from the exact bytes their `inputs` pin, so their existing claims
rest on a real rerun of the recorded selection instead of an artifact that no
longer describes its pins. The reruns make no new capability claim; the
delivered-row set stays at 577.

## Remaining boundary

- The gate-placement change itself is `49f2a3a7a`; this pass only rebinds
  receipts to it. It neither re-audits #1249's build-phase argument nor claims
  anything about the Gate's runtime behavior beyond the PASS recorded above.
- `CMakeLists.txt` and the seven test-shard modules are pinned by other
  receipts that did not pin them, and by receipts outside this set only if
  their own `inputs` list them; preflight reports zero drift for the entire
  835-input manifest, so no further receipt is stale as of this tree.
- The preflight's current-checkout `unknown` is a pre-existing environment
  condition (tracked symlink, explicit `core.symlinks` unset). It is recorded
  here because it keeps the tool's overall exit status at 1 even when the
  receipt line is green; it does not affect receipt classification.
