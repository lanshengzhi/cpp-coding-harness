# #1232 close-out: two drifted receipts rebound, evidence gate and full suite green (2026-11-12)

## Scope

Close-out of issue #1232 (the C++ safety and standards review of the Durable
EnvIO and Codemode slices). Its four items landed across earlier lanes on
`lane-preflight-receipt-drift`; what remained was the delivery pass: two
receipts whose pinned inputs no longer matched the tree, then the evidence gate
and one unfiltered full-suite run.

The drift is one file. `tests/codemode/CodemodeOwnerTest.cpp` was pinned as
`da1575359133…` by `1179-codemode-comparisons` and
`1192-codemode-tool-table-close`, and the #1232 work changed it to `a7b2ef9658…`:
`58cecf53e` replaces the run-result `std::stod` reader with a `from_chars`
reader, adds that reader's rejection case, and is the last commit to touch the
file. (`b81daa426`, HEAD here, formats only
`tests/durable/EnvIoListingDifferentialTest.cpp`, which neither receipt pins.)
Every other input
pinned by the two receipts still matched, and no other record drifted:
`scripts/dev/lane-preflight.py` reported *committed drift 2, uncommitted drift 0,
missing 0; paths: tests/codemode/CodemodeOwnerTest.cpp; tickets 1179 1192*.

Checkout `b81daa42673e94a35bd24a0584e87e42e3bb2345` on
`lane-preflight-receipt-drift`, working tree clean at the start, lane
`receipt-rebind-1232` taken with `scripts/dev/with-lane.sh` (which also exported
`CCH_LANE_NAME` and released the lock), Debug `vcpkg` preset in `build/`.

## Delivered behavior

- Selection reuse: each receipt's own `validation.command` was read back from
  `1058-deliveries.json` and its selector was passed to CTest verbatim — the
  52-case anchored `-R` expression of `1179-codemode-comparisons`, and the
  `-LE architecture` plus anchored 5-case `-R` expression of
  `1192-codemode-tool-table-close` — with `-j 1` from the receipt, plus
  `--no-tests=error` and `--output-on-failure`. No selector was retyped,
  widened or narrowed.
- Count check before each run: `ctest --preset vcpkg -N` on the same selector
  reported exactly `Total Tests: 52` and `Total Tests: 5`, matching each
  receipt's recorded `selected`. Both probes exited 0.
- Both selections passed: 52/52 and 5/5, zero skipped, zero failed. New
  artifacts are
  `deliveries/1179-codemode-comparisons-revalidation-1232-closeout.xml`
  (SHA-256 `c8c981129c972f4a2bf79dc408868905c28f72d4375f6a031fb6cf557ebf02d7`,
  `<testsuite tests="52" failures="0" disabled="0" skipped="0">`, 52 cases
  `status="run"`) and
  `deliveries/1192-codemode-tool-table-close-revalidation-1232-closeout.xml`
  (SHA-256 `a8d2fbbed262e3995c9b4c8555b8b57aaa131961117955c7ba111df6a82b5b1f`,
  `<testsuite tests="5" failures="0" disabled="0" skipped="0">`, 5 cases
  `status="run"`).
- Receipt updates, and nothing else. For each record the top-level `validation`
  block gains the new `junit.path` and `junit.sha256`, the unchanged `selected`,
  `testedRevision` `b81daa42673e94a35bd24a0584e87e42e3bb2345` (the checkout the
  runs happened on) and `recordedAt` `2026-11-11T17:30:50Z`; its `command` names
  the new artifact and records the `--output-on-failure` flag the run used.
  Exactly one input digest per record is rebound —
  `tests/codemode/CodemodeOwnerTest.cpp` — because a receipt whose pinned digest
  no longer matches the tree is rejected by `consolidate_audits.py` itself. No
  other input was rehashed, and `rows`, `routingSha256`,
  `implementationCommits` and `report` are byte-identical.
- History preserved: each record gains a `revalidation1232Closeout` entry whose
  `changedInputs` is `["tests/codemode/CodemodeOwnerTest.cpp"]` and whose
  `previousValidation` is the pre-rerun validation block verbatim (old JUnit
  path and digest, old `testedRevision`, old `recordedAt`). Earlier
  `previousValidation` chains under the records' other revalidation entries are
  untouched, and both superseded artifacts remain in `deliveries/` as history
  rather than as evidence.
- Artifact-path note, recorded because it is reproducible and was measured:
  CTest resolves a relative `--output-junit` path against the test preset's
  binary directory (`build/`), so the recorded `command` carries the absolute
  source-tree path that the run actually used, and `junit.path` stays
  repository-relative for the verifier. The receipt's `command` list is
  descriptive — consolidation never executes it.
- `consolidate_audits.py build` rewrote
  `1058-consolidated-capability-ledger.json` (5,886,621 bytes, 1464 rows); the
  ledger diff is the same two record blocks and nothing else.

## Validation

Every command below ran inside the lane lock with `CCH_LANE_NAME=receipt-rebind-1232`
exported, and every CTest invocation carried `TERM_PROGRAM=herdr`.

| Step | Command | rc | Measured |
| --- | --- | --- | --- |
| Incremental build | `cmake --build --preset vcpkg` | 0 | dry run ahead of it showed the build-phase Parity Architecture Gate as the only pending edge (1/1) |
| Count probe, 1179 | `ctest --preset vcpkg -N -R '<receipt selector>'` | 0 | `Total Tests: 52` = receipt `selected` 52 |
| Count probe, 1192 | `ctest --preset vcpkg -N -LE architecture -R '<receipt selector>'` | 0 | `Total Tests: 5` = receipt `selected` 5 |
| Rerun, 1179 | receipt command, 52-case `-R`, `-j 1` | 0 | 52 selected, 52 passed, 0 skipped, 0 failed, 103.94 s |
| Rerun, 1192 | receipt command, `-LE architecture`, 5-case `-R`, `-j 1` | 0 | 5 selected, 5 passed, 0 skipped, 0 failed, 4.69 s |
| Ledger build | `consolidate_audits.py build` | 0 | wrote the ledger: 5,886,621 bytes, 1464 rows |
| Ledger check | `consolidate_audits.py check` | 0 | 1464 source rows aliased into 1464 consolidated rows; 577 rows backed by pinned C++ comparison results; 1009 scenarios, 591 named by a row, 87 covered by a referenced artifact, 331 explicit orphans, 0 uncovered; 38 relative links resolve |
| Consolidation audit tests | `python3 tests/evidence/ConsolidationAuditTest.py` | 0 | 94 tests, 44.936 s |
| Evidence verification | `python3 scripts/evidence/audit_evidence.py verify --baseline pi-v1.3.0` | 0 | 84 bundles, 180 artifacts, 1012 scenarios at baseline `71023aec1546d6d677a65669c5665c0927ce0dd0` |
| Formatting gate | `scripts/format-check.sh origin/main` | 0 | clean |
| Full suite | `TERM_PROGRAM=herdr ctest --preset vcpkg -j2` | 0 | 4607 selected, 4596 passed, 11 skipped, 0 failed, 401.88 s |

The full suite is unfiltered: no label was excluded to reach green. The eleven
skips self-select at test level through their documented environment
preconditions — nine pi↔C++ dual-runtime continuation captures that need a
separately selected capture environment, the realtime-clock tool-duration case,
and the Native TUI differential capture. They are the same cases earlier
records report as skipped, and none of them covers the two receipts rebound
here.

One full-run failure that was **not** caused by this lane's edits had to be
diagnosed before the suite went green, and it is recorded rather than smoothed
over. The first full run selected the same 4607 cases and failed one:
`staged install contains only the relocatable Runtime and behaves after
relocation` (test 4552) exited 1 from the install gate with

```
install freshness: stale Runtime binary: 'build/pike' is older than link-closure
object 'CMakeFiles/cch_durable.dir/src/durable/CodingTools.cpp.o'; rebuild before install
```

Cause: an earlier lane's targeted build had recompiled that object at 02:07,
while `build/pike` was last linked at 01:05. `cch_durable` is not on pike's link
line, so ninja never relinks pike for it (`ninja -C build -n pike` — "no work to
do"), while `ninja -t inputs pike` — the closure the freshness check reads —
reaches it through pike's order-only `cch_parity_production_ready` dependency.
`build/pike` contains zero `cch::durable` symbols, so the binary was not
carrying stale code; the gate's closure is wider than the link line. The repair
is the one the diagnostic names: `cmake --build --preset vcpkg --target pike`
after removing the stale binary (rc 0, 15 s). Afterwards the closure had no
object newer than `build/pike`, the case passed alone (rc 0, 2.24 s), and the
full suite above passed. No source, test, bound or label changed for it.

## Capability status

No capability row, comparison scenario, discriminating case, `implementationCommits`
or routing digest changes. Both receipts again pin a JUnit file produced from the
exact bytes their `inputs` pin, so their existing claims rest on a real rerun of
the recorded selection instead of a stale artifact. Both reruns make no new
capability claim, and the ledger's delivered-row set is unchanged (577 rows).

The four #1232 items and where each landed on this branch:

| #1232 item | Landing |
| --- | --- |
| 1. `DaemonClient` raw pipe FDs and the leaked child (§7.8): `UniqueFd` ownership plus `waitpid` reaping | `33107f7da` wraps the pipe ends in `support::UniqueFd`; `c774ab100` reaps the forked daemon instead of leaking it (zombie and running-child leak); `b81daa426` formats those cases to the CI check |
| 2. Throwing number parsers under `-fno-exceptions` (§9.3, §5.7): `std::strtod`, `std::stoull`, `std::stod` → `from_chars` / `read_json` | `ee051020f` (`CodemodeRemoteTest.cpp`), `bae3a2b58` (`EnvIoReadToolTest.cpp`, `EnvIoListingTest.cpp` via `tests/support/PeakMemory.hpp`), `58cecf53e` (`EnvIoListingDifferentialTest.cpp`, `CodemodeOwnerTest.cpp`) |
| 3. `format_size` through `std::snprintf` into a raw buffer (§9.1, §16.2) | `e81e1cb99` renders the frozen size diagnostic with `std::format` (`{:.1f}{}`) including the JavaScript half-up rounding, covered by the `issue1232` oversized-line case in `EnvIoReadToolTest.cpp` |
| 4. `std::thread` in `RemoteDeterministicServe` (§9.13) | Not converted to `std::jthread`. `7d7411735` moved the handle out of the shared state into the frame-local `ScopedDeterministicWorker` and records the reason in source: a `jthread`'s `request_stop()` signals only its own stop source, not the run's `state.stop`, and its destructor would hide the request-stop-then-join ordering the owner must perform on the frame thread. The analysis behind that divergence is the wiki record's "Remote self-join root cause" and frame-join sections |

## Remaining boundary

- Item 4 is a deliberate, in-source-recorded divergence from the issue's
  suggested fix, not the suggested fix. Closing #1232 on that item is an owner
  ruling on the recorded divergence; this lane does not convert it into a
  "done as specified" claim.
- `src/coding_agent/mcp/McpConfigWrite.cpp:398` still calls `std::strtod`. It is
  outside the sites #1232 enumerates, no receipt pins it, and this lane did not
  change it; it is listed here as an observation, not as acceptance of a
  remaining obligation.
- The install-freshness closure described above is wider than pike's link line,
  so any targeted rebuild of a production library pike does not link can wedge
  the staged-install case until pike is relinked. That behaviour belongs to
  `cmake/install/check_install_freshness.py` (issue #1044's closure rule), not
  to this close-out; it is recorded here because it cost one full run.
